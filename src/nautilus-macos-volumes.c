/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* Mounts under /Volumes on macOS: external and network drives, disk images
 * and shares mounted with NetFS. GLib's Unix volume monitor considers them
 * system mounts and hides them, so without this they don't appear in the
 * sidebar and can't be ejected.
 *
 * This adds a volume monitor to GIO's "gio-volume-monitor" extension point,
 * which GVolumeMonitor combines with the built-in ones.
 */

#define G_LOG_DOMAIN "nautilus-macos-volumes"

#include "nautilus-macos-volumes.h"

#include <gio/gunixmounts.h>
#include <glib/gi18n.h>
#include <errno.h>
#include <glib-unix.h>
#include <sys/event.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

static gboolean
is_network_fs_type (const char *fs_type)
{
    static const char * const network_types[] = {
        "smbfs", "afpfs", "nfs", "webdav", "ftp", "cifs", NULL
    };

    return fs_type != NULL && g_strv_contains (network_types, fs_type);
}

/* ------------------------------------------------------------------------ */
/* Mount                                                                    */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_MOUNT (nautilus_macos_mount_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosMount, nautilus_macos_mount, NAUTILUS, MACOS_MOUNT, GObject)

struct _NautilusMacosMount
{
    GObject parent_instance;

    char *path;
    char *name;
    char *fs_type;
    char *sort_key;
    GVolumeMonitor *monitor;
};

static void mount_iface_init (GMountIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusMacosMount, nautilus_macos_mount, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, mount_iface_init))

static void
nautilus_macos_mount_finalize (GObject *object)
{
    NautilusMacosMount *self = NAUTILUS_MACOS_MOUNT (object);

    g_free (self->path);
    g_free (self->name);
    g_free (self->fs_type);
    g_free (self->sort_key);
    g_clear_weak_pointer (&self->monitor);

    G_OBJECT_CLASS (nautilus_macos_mount_parent_class)->finalize (object);
}

static void
nautilus_macos_mount_class_init (NautilusMacosMountClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = nautilus_macos_mount_finalize;
}

static void
nautilus_macos_mount_init (NautilusMacosMount *self)
{
}

static NautilusMacosMount *
nautilus_macos_mount_new (GVolumeMonitor *monitor,
                          const char     *path,
                          const char     *fs_type)
{
    NautilusMacosMount *self = g_object_new (NAUTILUS_TYPE_MACOS_MOUNT, NULL);

    self->path = g_strdup (path);
    /* Finder shows the volume name, which is the mount point's name */
    self->name = g_path_get_basename (path);
    self->fs_type = g_strdup (fs_type);
    self->sort_key = g_utf8_casefold (self->name, -1);
    g_set_weak_pointer (&self->monitor, monitor);

    return self;
}

static GFile *
macos_mount_get_root (GMount *mount)
{
    return g_file_new_for_path (NAUTILUS_MACOS_MOUNT (mount)->path);
}

static char *
macos_mount_get_name (GMount *mount)
{
    return g_strdup (NAUTILUS_MACOS_MOUNT (mount)->name);
}

static GIcon *
macos_mount_get_icon (GMount *mount)
{
    NautilusMacosMount *self = NAUTILUS_MACOS_MOUNT (mount);

    return g_themed_icon_new_with_default_fallbacks (is_network_fs_type (self->fs_type) ?
                                                     "folder-remote" : "drive-removable-media");
}

static GIcon *
macos_mount_get_symbolic_icon (GMount *mount)
{
    NautilusMacosMount *self = NAUTILUS_MACOS_MOUNT (mount);

    return g_themed_icon_new_with_default_fallbacks (is_network_fs_type (self->fs_type) ?
                                                     "folder-remote-symbolic" :
                                                     "drive-removable-media-symbolic");
}

static char *
macos_mount_get_uuid (GMount *mount)
{
    return NULL;
}

static GVolume *
macos_mount_get_volume (GMount *mount)
{
    return NULL;
}

static GDrive *
macos_mount_get_drive (GMount *mount)
{
    return NULL;
}

static gboolean
macos_mount_can_unmount (GMount *mount)
{
    return TRUE;
}

static gboolean
macos_mount_can_eject (GMount *mount)
{
    return TRUE;
}

static const char *
macos_mount_get_sort_key (GMount *mount)
{
    return NAUTILUS_MACOS_MOUNT (mount)->sort_key;
}

static void
diskutil_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
    g_autoptr (GTask) task = user_data;
    g_autoptr (GError) error = NULL;
    g_autofree char *errors = NULL;

    if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), result, NULL, &errors, &error))
    {
        g_task_return_error (task, g_steal_pointer (&error));
        return;
    }

    if (!g_subprocess_get_successful (G_SUBPROCESS (source)))
    {
        NautilusMacosMount *self = g_task_get_source_object (task);

        /* diskutil explains why, e.g. when files on the volume are open */
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_BUSY,
                                 _("Unable to eject “%s”: %s"), self->name,
                                 errors != NULL && errors[0] != '\0' ? g_strstrip (errors) :
                                 _("the volume is in use"));
        return;
    }

    g_task_return_boolean (task, TRUE);
}

/* Unmounts (or ejects the drive) with diskutil, as Finder does */
static void
run_diskutil (NautilusMacosMount  *self,
              const char          *verb,
              GCancellable        *cancellable,
              GAsyncReadyCallback  callback,
              gpointer             user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);
    g_autoptr (GError) error = NULL;
    g_autoptr (GSubprocess) process = NULL;

    process = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                &error, "/usr/sbin/diskutil", verb, self->path, NULL);
    if (process == NULL)
    {
        g_task_return_error (task, g_steal_pointer (&error));
        g_object_unref (task);
        return;
    }

    g_subprocess_communicate_utf8_async (process, NULL, cancellable, diskutil_done, task);
}

static void
macos_mount_unmount_with_operation (GMount              *mount,
                                    GMountUnmountFlags   flags,
                                    GMountOperation     *mount_operation,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
    run_diskutil (NAUTILUS_MACOS_MOUNT (mount), "unmount", cancellable, callback, user_data);
}

static void
macos_mount_eject_with_operation (GMount              *mount,
                                  GMountUnmountFlags   flags,
                                  GMountOperation     *mount_operation,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    NautilusMacosMount *self = NAUTILUS_MACOS_MOUNT (mount);

    /* Network shares have no drive to eject */
    run_diskutil (self, is_network_fs_type (self->fs_type) ? "unmount" : "eject",
                  cancellable, callback, user_data);
}

static gboolean
macos_mount_operation_finish (GMount        *mount,
                              GAsyncResult  *result,
                              GError       **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

static void
mount_iface_init (GMountIface *iface)
{
    iface->get_root = macos_mount_get_root;
    iface->get_name = macos_mount_get_name;
    iface->get_icon = macos_mount_get_icon;
    iface->get_symbolic_icon = macos_mount_get_symbolic_icon;
    iface->get_uuid = macos_mount_get_uuid;
    iface->get_volume = macos_mount_get_volume;
    iface->get_drive = macos_mount_get_drive;
    iface->can_unmount = macos_mount_can_unmount;
    iface->can_eject = macos_mount_can_eject;
    iface->get_sort_key = macos_mount_get_sort_key;
    iface->unmount_with_operation = macos_mount_unmount_with_operation;
    iface->unmount_with_operation_finish = macos_mount_operation_finish;
    iface->eject_with_operation = macos_mount_eject_with_operation;
    iface->eject_with_operation_finish = macos_mount_operation_finish;
}

/* ------------------------------------------------------------------------ */
/* Volume monitor                                                           */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_VOLUME_MONITOR (nautilus_macos_volume_monitor_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosVolumeMonitor, nautilus_macos_volume_monitor,
                      NAUTILUS, MACOS_VOLUME_MONITOR, GVolumeMonitor)

struct _NautilusMacosVolumeMonitor
{
    GVolumeMonitor parent_instance;

    /* GLib's GUnixMountMonitor doesn't notice mounts on macOS: the
     * kernel reports them to a kqueue */
    int kqueue_fd;
    guint kqueue_watch;
    GList *mounts;
};

G_DEFINE_FINAL_TYPE (NautilusMacosVolumeMonitor, nautilus_macos_volume_monitor, G_TYPE_VOLUME_MONITOR)

/* Volumes Finder shows: under /Volumes, and not hidden (MNT_DONTBROWSE is
 * set on e.g. the Recovery and Preboot system volumes) */
static gboolean
is_user_volume (GUnixMountEntry *entry)
{
    const char *path = g_unix_mount_entry_get_mount_path (entry);
    struct statfs buf;

    if (!g_str_has_prefix (path, "/Volumes/"))
    {
        return FALSE;
    }

    if (statfs (path, &buf) != 0)
    {
        return FALSE;
    }

    return (buf.f_flags & MNT_DONTBROWSE) == 0;
}

static NautilusMacosMount *
find_mount (GList      *mounts,
            const char *path)
{
    for (GList *l = mounts; l != NULL; l = l->next)
    {
        if (g_str_equal (NAUTILUS_MACOS_MOUNT (l->data)->path, path))
        {
            return l->data;
        }
    }

    return NULL;
}

static void
update_mounts (NautilusMacosVolumeMonitor *self)
{
    g_autolist (GUnixMountEntry) entries = NULL;
    GList *new_mounts = NULL;
    GList *added = NULL;
    GList *removed = NULL;

    entries = g_unix_mount_entries_get (NULL);

    for (GList *l = entries; l != NULL; l = l->next)
    {
        GUnixMountEntry *entry = l->data;
        const char *path = g_unix_mount_entry_get_mount_path (entry);
        NautilusMacosMount *mount;

        if (!is_user_volume (entry) || find_mount (new_mounts, path) != NULL)
        {
            continue;
        }

        mount = find_mount (self->mounts, path);
        if (mount != NULL)
        {
            new_mounts = g_list_prepend (new_mounts, g_object_ref (mount));
        }
        else
        {
            mount = nautilus_macos_mount_new (G_VOLUME_MONITOR (self), path,
                                              g_unix_mount_entry_get_fs_type (entry));
            new_mounts = g_list_prepend (new_mounts, mount);
            added = g_list_prepend (added, g_object_ref (mount));
        }
    }

    for (GList *l = self->mounts; l != NULL; l = l->next)
    {
        if (find_mount (new_mounts, NAUTILUS_MACOS_MOUNT (l->data)->path) == NULL)
        {
            removed = g_list_prepend (removed, g_object_ref (l->data));
        }
    }

    g_list_free_full (self->mounts, g_object_unref);
    self->mounts = new_mounts;

    for (GList *l = removed; l != NULL; l = l->next)
    {
        g_signal_emit_by_name (l->data, "unmounted");
        g_signal_emit_by_name (self, "mount-removed", l->data);
    }
    for (GList *l = added; l != NULL; l = l->next)
    {
        g_signal_emit_by_name (self, "mount-added", l->data);
    }

    g_list_free_full (added, g_object_unref);
    g_list_free_full (removed, g_object_unref);
}

static gboolean
on_mounts_changed (int          fd,
                   GIOCondition condition,
                   gpointer     user_data)
{
    struct kevent event;
    struct timespec no_wait = { 0, 0 };

    /* Mounting may post several events at once */
    while (kevent (fd, NULL, 0, &event, 1, &no_wait) > 0)
    {
    }

    update_mounts (user_data);

    return G_SOURCE_CONTINUE;
}

static GList *
monitor_get_mounts (GVolumeMonitor *monitor)
{
    NautilusMacosVolumeMonitor *self = NAUTILUS_MACOS_VOLUME_MONITOR (monitor);

    return g_list_copy_deep (self->mounts, (GCopyFunc) g_object_ref, NULL);
}

static GList *
monitor_get_empty_list (GVolumeMonitor *monitor)
{
    return NULL;
}

static GVolume *
monitor_get_volume_for_uuid (GVolumeMonitor *monitor,
                             const char     *uuid)
{
    return NULL;
}

static GMount *
monitor_get_mount_for_uuid (GVolumeMonitor *monitor,
                            const char     *uuid)
{
    return NULL;
}

static gboolean
monitor_is_supported (void)
{
    return TRUE;
}

static void
nautilus_macos_volume_monitor_dispose (GObject *object)
{
    NautilusMacosVolumeMonitor *self = NAUTILUS_MACOS_VOLUME_MONITOR (object);

    g_clear_handle_id (&self->kqueue_watch, g_source_remove);
    if (self->kqueue_fd >= 0)
    {
        close (self->kqueue_fd);
        self->kqueue_fd = -1;
    }
    g_list_free_full (g_steal_pointer (&self->mounts), g_object_unref);

    G_OBJECT_CLASS (nautilus_macos_volume_monitor_parent_class)->dispose (object);
}

static void
nautilus_macos_volume_monitor_class_init (NautilusMacosVolumeMonitorClass *klass)
{
    GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);

    G_OBJECT_CLASS (klass)->dispose = nautilus_macos_volume_monitor_dispose;
    monitor_class->is_supported = monitor_is_supported;
    monitor_class->get_mounts = monitor_get_mounts;
    monitor_class->get_volumes = monitor_get_empty_list;
    monitor_class->get_connected_drives = monitor_get_empty_list;
    monitor_class->get_volume_for_uuid = monitor_get_volume_for_uuid;
    monitor_class->get_mount_for_uuid = monitor_get_mount_for_uuid;
}

static void
nautilus_macos_volume_monitor_init (NautilusMacosVolumeMonitor *self)
{
    struct kevent filter;

    self->kqueue_fd = kqueue ();
    EV_SET (&filter, 0, EVFILT_FS, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (self->kqueue_fd >= 0 && kevent (self->kqueue_fd, &filter, 1, NULL, 0, NULL) == 0)
    {
        self->kqueue_watch = g_unix_fd_add (self->kqueue_fd, G_IO_IN, on_mounts_changed, self);
    }
    else
    {
        g_warning ("Can't watch for mounted volumes: %s", g_strerror (errno));
    }

    update_mounts (self);
}

/**
 * nautilus_macos_volumes_register:
 *
 * Must be called before anything uses g_volume_monitor_get().
 */
void
nautilus_macos_volumes_register (void)
{
    /* Makes GIO register its extension points before adding to one */
    g_vfs_get_default ();

    g_io_extension_point_implement (G_VOLUME_MONITOR_EXTENSION_POINT_NAME,
                                    NAUTILUS_TYPE_MACOS_VOLUME_MONITOR,
                                    "nautilus-macos", 0);
}
