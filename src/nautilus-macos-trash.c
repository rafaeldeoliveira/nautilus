/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The trash:// location on macOS, where GVfs (which provides it on Linux)
 * is not available.
 *
 * trash:///NAME maps to ~/.Trash/NAME, where macOS's own trash (and
 * g_file_trash(), which uses NSFileManager) puts files. Reading ~/.Trash
 * requires Full Disk Access; without it the location cannot be listed and
 * Nautilus falls back to Finder (see nautilus_macos_trash_is_readable()).
 *
 * Like GVfs's trash backend, top-level items carry trash::deletion-date
 * (from the date macOS recorded the item was added to the folder) and,
 * for items Nautilus trashed, trash::orig-path (from an extended
 * attribute set before trashing), and deleting one removes it entirely.
 */

#define G_LOG_DOMAIN "nautilus-macos-trash"

#include "nautilus-macos-trash.h"

#include <errno.h>
#include <dirent.h>
#include <glib/gi18n.h>
#include <removefile.h>
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define TRASH_SCHEME "trash"

static char *
trash_dir_path (void)
{
    return g_build_filename (g_get_home_dir (), ".Trash", NULL);
}

static gboolean
is_hidden_metadata (const char *name)
{
    /* Finder's view settings for the Trash, not a trashed item */
    return g_str_equal (name, ".DS_Store");
}

static char *
read_origin (const char *path)
{
    char buffer[4096];
    ssize_t size = getxattr (path, NAUTILUS_MACOS_TRASH_ORIGIN_XATTR,
                             buffer, sizeof (buffer) - 1, 0, XATTR_NOFOLLOW);

    if (size <= 0)
    {
        return NULL;
    }

    buffer[size] = '\0';

    return g_strdup (buffer);
}

/* When the item was moved into the trash folder */
static char *
read_deletion_date (const char *path)
{
    char recorded[32];
    ssize_t recorded_size;
    struct attrlist attrs = {
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .commonattr = ATTR_CMN_ADDEDTIME,
    };
    struct
    {
        u_int32_t length;
        struct timespec added;
    } __attribute__ ((aligned (4), packed)) buffer;
    g_autoptr (GDateTime) date = NULL;

    /* Recorded by Nautilus when it trashed the item; exact, so that undoing
     * the operation can match it */
    recorded_size = getxattr (path, NAUTILUS_MACOS_TRASH_TIME_XATTR,
                              recorded, sizeof (recorded) - 1, 0, XATTR_NOFOLLOW);
    if (recorded_size > 0)
    {
        recorded[recorded_size] = '\0';
        date = g_date_time_new_from_unix_local (g_ascii_strtoll (recorded, NULL, 10));
    }
    else if (getattrlist (path, &attrs, &buffer, sizeof (buffer), FSOPT_NOFOLLOW) == 0)
    {
        /* Otherwise, when the item was added to the trash folder */
        date = g_date_time_new_from_unix_local (buffer.added.tv_sec);
    }

    /* Local time without a zone, as GVfs and g_file_info_get_deletion_date() expect */
    return date != NULL ? g_date_time_format (date, "%Y-%m-%dT%H:%M:%S") : NULL;
}

static guint32
count_items (void)
{
    g_autofree char *path = trash_dir_path ();
    DIR *dir = opendir (path);
    struct dirent *entry;
    guint32 count = 0;

    if (dir == NULL)
    {
        return 0;
    }

    while ((entry = readdir (dir)) != NULL)
    {
        if (!g_str_equal (entry->d_name, ".") &&
            !g_str_equal (entry->d_name, "..") &&
            !is_hidden_metadata (entry->d_name))
        {
            count++;
        }
    }

    closedir (dir);

    return count;
}

/* Turns information about a file inside ~/.Trash into information about
 * the corresponding trash:// item. */
static void
adjust_info (GFileInfo  *info,
             const char *path,
             gboolean    toplevel)
{
    /* Infos from local files are limited to the requested attributes */
    g_file_info_unset_attribute_mask (info);

    /* Trashed items can only be restored or deleted */
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, toplevel);

    if (toplevel)
    {
        g_autofree char *origin = read_origin (path);
        g_autofree char *date = read_deletion_date (path);

        if (origin != NULL)
        {
            g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH, origin);
        }
        if (date != NULL)
        {
            g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_TRASH_DELETION_DATE, date);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* File                                                                     */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_TRASH_FILE (nautilus_macos_trash_file_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosTrashFile, nautilus_macos_trash_file,
                      NAUTILUS, MACOS_TRASH_FILE, GObject)

struct _NautilusMacosTrashFile
{
    GObject parent_instance;

    /* Path inside the trash: "" for the trash itself, "NAME", "NAME/CHILD"… */
    char *relative;
};

static void file_iface_init (GFileIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusMacosTrashFile, nautilus_macos_trash_file, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_FILE, file_iface_init))

static void
nautilus_macos_trash_file_finalize (GObject *object)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (object);

    g_free (self->relative);

    G_OBJECT_CLASS (nautilus_macos_trash_file_parent_class)->finalize (object);
}

static void
nautilus_macos_trash_file_class_init (NautilusMacosTrashFileClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = nautilus_macos_trash_file_finalize;
}

static void
nautilus_macos_trash_file_init (NautilusMacosTrashFile *self)
{
}

/* Normalizes @relative: no empty, "." or leading/trailing components */
static GFile *
trash_file_new (const char *relative)
{
    NautilusMacosTrashFile *self = g_object_new (NAUTILUS_TYPE_MACOS_TRASH_FILE, NULL);
    g_auto (GStrv) parts = g_strsplit (relative != NULL ? relative : "", "/", -1);
    g_autoptr (GPtrArray) kept = g_ptr_array_new ();

    for (guint i = 0; parts[i] != NULL; i++)
    {
        if (parts[i][0] == '\0' || g_str_equal (parts[i], "."))
        {
            continue;
        }

        if (g_str_equal (parts[i], ".."))
        {
            if (kept->len > 0)
            {
                g_ptr_array_remove_index (kept, kept->len - 1);
            }
            continue;
        }

        g_ptr_array_add (kept, parts[i]);
    }

    g_ptr_array_add (kept, NULL);
    self->relative = g_strjoinv ("/", (char **) kept->pdata);

    return G_FILE (self);
}

static gboolean
is_root (NautilusMacosTrashFile *self)
{
    return self->relative[0] == '\0';
}

static gboolean
is_toplevel (NautilusMacosTrashFile *self)
{
    return !is_root (self) && strchr (self->relative, '/') == NULL;
}

static char *
real_path (NautilusMacosTrashFile *self)
{
    g_autofree char *dir = trash_dir_path ();

    return is_root (self) ? g_steal_pointer (&dir) : g_build_filename (dir, self->relative, NULL);
}

static GFile *
real_file (NautilusMacosTrashFile *self)
{
    g_autofree char *path = real_path (self);

    return g_file_new_for_path (path);
}

static GFile *
trash_file_dup (GFile *file)
{
    return trash_file_new (NAUTILUS_MACOS_TRASH_FILE (file)->relative);
}

static guint
trash_file_hash (GFile *file)
{
    return g_str_hash (NAUTILUS_MACOS_TRASH_FILE (file)->relative);
}

static gboolean
trash_file_equal (GFile *file1,
                  GFile *file2)
{
    return NAUTILUS_IS_MACOS_TRASH_FILE (file2) &&
           g_str_equal (NAUTILUS_MACOS_TRASH_FILE (file1)->relative,
                        NAUTILUS_MACOS_TRASH_FILE (file2)->relative);
}

static gboolean
trash_file_is_native (GFile *file)
{
    return FALSE;
}

static gboolean
trash_file_has_uri_scheme (GFile      *file,
                           const char *uri_scheme)
{
    return g_ascii_strcasecmp (uri_scheme, TRASH_SCHEME) == 0;
}

static char *
trash_file_get_uri_scheme (GFile *file)
{
    return g_strdup (TRASH_SCHEME);
}

static char *
trash_file_get_basename (GFile *file)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);

    return is_root (self) ? g_strdup ("/") : g_path_get_basename (self->relative);
}

static char *
trash_file_get_path (GFile *file)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);

    /* Lets applications open trashed files */
    return is_root (self) ? NULL : real_path (self);
}

static char *
trash_file_get_uri (GFile *file)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);
    g_autofree char *escaped = g_uri_escape_string (self->relative,
                                                    G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
                                                    FALSE);

    return g_strconcat (TRASH_SCHEME ":///", escaped, NULL);
}

static GFile *
trash_file_get_parent (GFile *file)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);
    g_autofree char *parent = NULL;

    if (is_root (self))
    {
        return NULL;
    }

    parent = g_path_get_dirname (self->relative);

    return trash_file_new (g_str_equal (parent, ".") ? "" : parent);
}

static gboolean
trash_file_prefix_matches (GFile *prefix,
                           GFile *file)
{
    const char *prefix_relative;
    const char *file_relative;
    gsize length;

    if (!NAUTILUS_IS_MACOS_TRASH_FILE (prefix) || !NAUTILUS_IS_MACOS_TRASH_FILE (file))
    {
        return FALSE;
    }

    prefix_relative = NAUTILUS_MACOS_TRASH_FILE (prefix)->relative;
    file_relative = NAUTILUS_MACOS_TRASH_FILE (file)->relative;
    length = strlen (prefix_relative);

    if (length == 0)
    {
        return file_relative[0] != '\0';
    }

    return g_str_has_prefix (file_relative, prefix_relative) && file_relative[length] == '/';
}

static char *
trash_file_get_relative_path (GFile *parent,
                              GFile *descendant)
{
    gsize length;

    if (!trash_file_prefix_matches (parent, descendant))
    {
        return NULL;
    }

    length = strlen (NAUTILUS_MACOS_TRASH_FILE (parent)->relative);

    return g_strdup (NAUTILUS_MACOS_TRASH_FILE (descendant)->relative + (length > 0 ? length + 1 : 0));
}

static GFile *
trash_file_resolve_relative_path (GFile      *file,
                                  const char *relative_path)
{
    g_autofree char *joined = NULL;

    if (g_path_is_absolute (relative_path))
    {
        return trash_file_new (relative_path);
    }

    joined = g_build_filename (NAUTILUS_MACOS_TRASH_FILE (file)->relative, relative_path, NULL);

    return trash_file_new (joined);
}

static GFile *
trash_file_get_child_for_display_name (GFile       *file,
                                       const char  *display_name,
                                       GError     **error)
{
    return trash_file_resolve_relative_path (file, display_name);
}

static GFileInfo *
root_info (void)
{
    GFileInfo *info = g_file_info_new ();
    guint32 count = count_items ();
    g_autoptr (GIcon) icon = g_themed_icon_new (count > 0 ? "user-trash-full" : "user-trash");
    g_autoptr (GIcon) symbolic_icon = g_themed_icon_new (count > 0 ? "user-trash-full-symbolic"
                                                                   : "user-trash-symbolic");

    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, _("Trash"));
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
    g_file_info_set_icon (info, icon);
    g_file_info_set_symbolic_icon (info, symbolic_icon);
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, count);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
    /* Dropping files on the trash trashes them */
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

    return info;
}

static GFileInfo *
trash_file_query_info (GFile                *file,
                       const char           *attributes,
                       GFileQueryInfoFlags   flags,
                       GCancellable         *cancellable,
                       GError              **error)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);
    g_autoptr (GFile) real = NULL;
    g_autofree char *path = NULL;
    GFileInfo *info;

    if (is_root (self))
    {
        return root_info ();
    }

    real = real_file (self);
    info = g_file_query_info (real, attributes, flags, cancellable, error);
    if (info != NULL)
    {
        path = real_path (self);
        adjust_info (info, path, is_toplevel (self));
    }

    return info;
}

static GFileInfo *
trash_file_query_filesystem_info (GFile         *file,
                                  const char    *attributes,
                                  GCancellable  *cancellable,
                                  GError       **error)
{
    g_autofree char *path = trash_dir_path ();
    g_autoptr (GFile) dir = g_file_new_for_path (path);

    return g_file_query_filesystem_info (dir, attributes, cancellable, error);
}

static gboolean
trash_file_delete (GFile         *file,
                   GCancellable  *cancellable,
                   GError       **error)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);
    g_autofree char *path = NULL;

    /* As with GVfs, only whole items can be deleted from the trash */
    if (!is_toplevel (self))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("Items inside trashed folders cannot be deleted separately"));
        return FALSE;
    }

    path = real_path (self);
    if (removefile (path, NULL, REMOVEFILE_RECURSIVE) != 0)
    {
        int saved_errno = errno;

        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                     _("Error deleting “%s”: %s"), self->relative, g_strerror (saved_errno));
        return FALSE;
    }

    return TRUE;
}

/* Restores an item, when moved back onto the same volume */
static gboolean
trash_file_move (GFile                  *source,
                 GFile                  *destination,
                 GFileCopyFlags          flags,
                 GCancellable           *cancellable,
                 GFileProgressCallback   progress_callback,
                 gpointer                progress_callback_data,
                 GError                **error)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (source);
    g_autofree char *source_path = NULL;
    g_autofree char *destination_path = NULL;
    struct stat buf;

    destination_path = g_file_get_path (destination);
    if (is_root (self) || destination_path == NULL || !g_file_is_native (destination))
    {
        /* Lets GIO copy and delete instead */
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Not supported");
        return FALSE;
    }

    if (!(flags & G_FILE_COPY_OVERWRITE) && lstat (destination_path, &buf) == 0)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                     _("“%s” already exists"), destination_path);
        return FALSE;
    }

    source_path = real_path (self);
    if (rename (source_path, destination_path) != 0)
    {
        int saved_errno = errno;

        if (saved_errno == EXDEV)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Not supported");
        }
        else
        {
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                         _("Error restoring “%s”: %s"), self->relative, g_strerror (saved_errno));
        }
        return FALSE;
    }

    removexattr (destination_path, NAUTILUS_MACOS_TRASH_ORIGIN_XATTR, XATTR_NOFOLLOW);
    removexattr (destination_path, NAUTILUS_MACOS_TRASH_TIME_XATTR, XATTR_NOFOLLOW);

    return TRUE;
}

static GFileInputStream *
trash_file_read (GFile         *file,
                 GCancellable  *cancellable,
                 GError       **error)
{
    g_autoptr (GFile) real = real_file (NAUTILUS_MACOS_TRASH_FILE (file));

    return g_file_read (real, cancellable, error);
}

/* ------------------------------------------------------------------------ */
/* Enumerator                                                               */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_TRASH_ENUMERATOR (nautilus_macos_trash_enumerator_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosTrashEnumerator, nautilus_macos_trash_enumerator,
                      NAUTILUS, MACOS_TRASH_ENUMERATOR, GFileEnumerator)

struct _NautilusMacosTrashEnumerator
{
    GFileEnumerator parent_instance;

    GFileEnumerator *real;
    char *directory_path;
    gboolean is_root;
};

G_DEFINE_FINAL_TYPE (NautilusMacosTrashEnumerator, nautilus_macos_trash_enumerator,
                     G_TYPE_FILE_ENUMERATOR)

static GFileInfo *
trash_enumerator_next_file (GFileEnumerator  *enumerator,
                            GCancellable     *cancellable,
                            GError          **error)
{
    NautilusMacosTrashEnumerator *self = NAUTILUS_MACOS_TRASH_ENUMERATOR (enumerator);
    GFileInfo *info;

    while ((info = g_file_enumerator_next_file (self->real, cancellable, error)) != NULL)
    {
        const char *name = g_file_info_get_name (info);
        g_autofree char *path = NULL;

        if (self->is_root && is_hidden_metadata (name))
        {
            g_object_unref (info);
            continue;
        }

        path = g_build_filename (self->directory_path, name, NULL);
        adjust_info (info, path, self->is_root);

        return info;
    }

    return NULL;
}

static gboolean
trash_enumerator_close (GFileEnumerator  *enumerator,
                        GCancellable     *cancellable,
                        GError          **error)
{
    NautilusMacosTrashEnumerator *self = NAUTILUS_MACOS_TRASH_ENUMERATOR (enumerator);

    return g_file_enumerator_close (self->real, cancellable, error);
}

static void
nautilus_macos_trash_enumerator_finalize (GObject *object)
{
    NautilusMacosTrashEnumerator *self = NAUTILUS_MACOS_TRASH_ENUMERATOR (object);

    g_clear_object (&self->real);
    g_free (self->directory_path);

    G_OBJECT_CLASS (nautilus_macos_trash_enumerator_parent_class)->finalize (object);
}

static void
nautilus_macos_trash_enumerator_class_init (NautilusMacosTrashEnumeratorClass *klass)
{
    GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);

    G_OBJECT_CLASS (klass)->finalize = nautilus_macos_trash_enumerator_finalize;
    enumerator_class->next_file = trash_enumerator_next_file;
    enumerator_class->close_fn = trash_enumerator_close;
}

static void
nautilus_macos_trash_enumerator_init (NautilusMacosTrashEnumerator *self)
{
}

static GFileEnumerator *
trash_file_enumerate_children (GFile                *file,
                               const char           *attributes,
                               GFileQueryInfoFlags   flags,
                               GCancellable         *cancellable,
                               GError              **error)
{
    NautilusMacosTrashFile *self = NAUTILUS_MACOS_TRASH_FILE (file);
    g_autoptr (GFile) real = real_file (self);
    GFileEnumerator *real_enumerator;
    NautilusMacosTrashEnumerator *enumerator;

    real_enumerator = g_file_enumerate_children (real, attributes, flags, cancellable, error);
    if (real_enumerator == NULL)
    {
        return NULL;
    }

    enumerator = g_object_new (NAUTILUS_TYPE_MACOS_TRASH_ENUMERATOR,
                               "container", file,
                               NULL);
    enumerator->real = real_enumerator;
    enumerator->directory_path = real_path (self);
    enumerator->is_root = is_root (self);

    return G_FILE_ENUMERATOR (enumerator);
}

/* ------------------------------------------------------------------------ */
/* Monitor                                                                  */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_TRASH_MONITOR (nautilus_macos_trash_monitor_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosTrashMonitor, nautilus_macos_trash_monitor,
                      NAUTILUS, MACOS_TRASH_MONITOR, GFileMonitor)

struct _NautilusMacosTrashMonitor
{
    GFileMonitor parent_instance;

    GFileMonitor *real;
    GFile *trash_dir;
};

G_DEFINE_FINAL_TYPE (NautilusMacosTrashMonitor, nautilus_macos_trash_monitor, G_TYPE_FILE_MONITOR)

static GFile *
trash_file_for_real (NautilusMacosTrashMonitor *self,
                     GFile                     *real)
{
    g_autofree char *relative = NULL;

    if (real == NULL)
    {
        return NULL;
    }

    if (g_file_equal (real, self->trash_dir))
    {
        return trash_file_new ("");
    }

    relative = g_file_get_relative_path (self->trash_dir, real);

    return relative != NULL ? trash_file_new (relative) : NULL;
}

static void
on_real_monitor_changed (GFileMonitor              *real,
                         GFile                     *file,
                         GFile                     *other_file,
                         GFileMonitorEvent          event,
                         NautilusMacosTrashMonitor *self)
{
    g_autoptr (GFile) trash_file = trash_file_for_real (self, file);
    g_autoptr (GFile) trash_other_file = trash_file_for_real (self, other_file);
    g_autofree char *basename = file != NULL ? g_file_get_basename (file) : NULL;

    if (trash_file == NULL || (basename != NULL && is_hidden_metadata (basename)))
    {
        return;
    }

    g_file_monitor_emit_event (G_FILE_MONITOR (self), trash_file, trash_other_file, event);
}

static gboolean
trash_monitor_cancel (GFileMonitor *monitor)
{
    NautilusMacosTrashMonitor *self = NAUTILUS_MACOS_TRASH_MONITOR (monitor);

    if (self->real != NULL)
    {
        g_file_monitor_cancel (self->real);
    }

    return TRUE;
}

static void
nautilus_macos_trash_monitor_dispose (GObject *object)
{
    NautilusMacosTrashMonitor *self = NAUTILUS_MACOS_TRASH_MONITOR (object);

    if (self->real != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->real, self);
        g_file_monitor_cancel (self->real);
        g_clear_object (&self->real);
    }
    g_clear_object (&self->trash_dir);

    G_OBJECT_CLASS (nautilus_macos_trash_monitor_parent_class)->dispose (object);
}

static void
nautilus_macos_trash_monitor_class_init (NautilusMacosTrashMonitorClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_macos_trash_monitor_dispose;
    G_FILE_MONITOR_CLASS (klass)->cancel = trash_monitor_cancel;
}

static void
nautilus_macos_trash_monitor_init (NautilusMacosTrashMonitor *self)
{
}

/* Never fails: without access to ~/.Trash the monitor just reports nothing */
static GFileMonitor *
trash_monitor_new (NautilusMacosTrashFile *file,
                   GFileMonitorFlags       flags,
                   GCancellable           *cancellable)
{
    NautilusMacosTrashMonitor *self = g_object_new (NAUTILUS_TYPE_MACOS_TRASH_MONITOR, NULL);
    g_autofree char *trash_path = trash_dir_path ();
    g_autoptr (GFile) real = real_file (file);

    self->trash_dir = g_file_new_for_path (trash_path);
    self->real = g_file_monitor (real, flags, cancellable, NULL);
    if (self->real != NULL)
    {
        g_signal_connect (self->real, "changed", G_CALLBACK (on_real_monitor_changed), self);
    }

    return G_FILE_MONITOR (self);
}

static GFileMonitor *
trash_file_monitor_dir (GFile              *file,
                        GFileMonitorFlags   flags,
                        GCancellable       *cancellable,
                        GError            **error)
{
    return trash_monitor_new (NAUTILUS_MACOS_TRASH_FILE (file), flags, cancellable);
}

static GFileMonitor *
trash_file_monitor_file (GFile              *file,
                         GFileMonitorFlags   flags,
                         GCancellable       *cancellable,
                         GError            **error)
{
    return trash_monitor_new (NAUTILUS_MACOS_TRASH_FILE (file), flags, cancellable);
}

static void
file_iface_init (GFileIface *iface)
{
    iface->dup = trash_file_dup;
    iface->hash = trash_file_hash;
    iface->equal = trash_file_equal;
    iface->is_native = trash_file_is_native;
    iface->has_uri_scheme = trash_file_has_uri_scheme;
    iface->get_uri_scheme = trash_file_get_uri_scheme;
    iface->get_basename = trash_file_get_basename;
    iface->get_path = trash_file_get_path;
    iface->get_uri = trash_file_get_uri;
    iface->get_parse_name = trash_file_get_uri;
    iface->get_parent = trash_file_get_parent;
    iface->prefix_matches = trash_file_prefix_matches;
    iface->get_relative_path = trash_file_get_relative_path;
    iface->resolve_relative_path = trash_file_resolve_relative_path;
    iface->get_child_for_display_name = trash_file_get_child_for_display_name;
    iface->enumerate_children = trash_file_enumerate_children;
    iface->query_info = trash_file_query_info;
    iface->query_filesystem_info = trash_file_query_filesystem_info;
    iface->delete_file = trash_file_delete;
    iface->move = trash_file_move;
    iface->read_fn = trash_file_read;
    iface->monitor_dir = trash_file_monitor_dir;
    iface->monitor_file = trash_file_monitor_file;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Also called for every parse name, whatever its scheme: only handle ours,
 * returning NULL lets GVfs handle the others (paths, other URIs). */
static GFile *
lookup_trash_uri (GVfs       *vfs,
                  const char *identifier,
                  gpointer    user_data)
{
    g_autofree char *relative = NULL;

    if (g_ascii_strncasecmp (identifier, TRASH_SCHEME ":", strlen (TRASH_SCHEME ":")) != 0)
    {
        return NULL;
    }

    relative = g_uri_unescape_string (identifier + strlen (TRASH_SCHEME ":"), NULL);
    if (relative == NULL)
    {
        return NULL;
    }

    return trash_file_new (relative);
}

/**
 * nautilus_macos_trash_register:
 *
 * Provides trash:// unless GVfs already does.
 */
void
nautilus_macos_trash_register (void)
{
    GVfs *vfs = g_vfs_get_default ();

    if (g_strv_contains (g_vfs_get_supported_uri_schemes (vfs), TRASH_SCHEME))
    {
        return;
    }

    g_vfs_register_uri_scheme (vfs, TRASH_SCHEME,
                               lookup_trash_uri, NULL, NULL,
                               lookup_trash_uri, NULL, NULL);
}

/**
 * nautilus_macos_trash_is_readable:
 *
 * Returns: whether the trash can be listed, which macOS only allows to
 *   applications with Full Disk Access
 */
gboolean
nautilus_macos_trash_is_readable (void)
{
    g_autofree char *path = trash_dir_path ();
    DIR *dir = opendir (path);

    if (dir == NULL)
    {
        return FALSE;
    }

    closedir (dir);

    return TRUE;
}

/**
 * nautilus_macos_trash_record_origin:
 * @file: a file about to be trashed
 *
 * Records where @file is, so it can be restored from the trash.
 */
void
nautilus_macos_trash_record_origin (GFile *file)
{
    g_autofree char *path = g_file_get_path (file);

    if (path != NULL)
    {
        g_autofree char *now = g_strdup_printf ("%" G_GINT64_FORMAT, g_get_real_time () / G_USEC_PER_SEC);

        setxattr (path, NAUTILUS_MACOS_TRASH_ORIGIN_XATTR, path, strlen (path), 0, XATTR_NOFOLLOW);
        setxattr (path, NAUTILUS_MACOS_TRASH_TIME_XATTR, now, strlen (now), 0, XATTR_NOFOLLOW);
    }
}

static void
spawn (const char * const *argv)
{
    g_autoptr (GError) error = NULL;

    if (!g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
    {
        g_warning ("Failed to run %s: %s", argv[0], error->message);
    }
}

/**
 * nautilus_macos_trash_open_in_finder:
 *
 * Shows the trash in Finder, which can always read it.
 */
void
nautilus_macos_trash_open_in_finder (void)
{
    g_autofree char *path = trash_dir_path ();
    const char * const argv[] = { "/usr/bin/open", "-b", "com.apple.finder", path, NULL };

    spawn (argv);
}

/**
 * nautilus_macos_trash_empty_with_finder:
 *
 * Asks Finder to empty the trash. macOS asks the user, the first time,
 * to allow Files to control Finder.
 */
void
nautilus_macos_trash_empty_with_finder (void)
{
    const char * const argv[] = {
        "/usr/bin/osascript", "-e", "tell application \"Finder\" to empty trash", NULL
    };

    spawn (argv);
}
