/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* The locations behind Nautilus's Network view, which GVfs provides on
 * Linux:
 *
 * - network:/// lists file servers advertised on the local network with
 *   Bonjour, as Finder's "Network" does. Each one is a shortcut to an
 *   address (smb://host.local/…) that macOS mounts when it is opened.
 * - computer:/// lists the mounted volumes, which the Network view shows
 *   under "Connected" when they are network shares.
 */

#define G_LOG_DOMAIN "nautilus-macos"

#include "nautilus-macos-network.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <string.h>

#define NETWORK_SCHEME "network"
#define COMPUTER_SCHEME "computer"

/* ------------------------------------------------------------------------ */
/* Bonjour discovery                                                        */
/* ------------------------------------------------------------------------ */

typedef struct
{
    const char *service_type;
    const char *scheme;
    guint16 default_port;
} ServiceKind;

/* Kinds of file servers macOS can mount, best first: a computer that
 * offers several is shown once, with the best one. */
static const ServiceKind service_kinds[] =
{
    { "_smb._tcp", "smb", 445 },
    { "_afpovertcp._tcp", "afp", 548 },
    { "_nfs._tcp", "nfs", 2049 },
    { "_webdavs._tcp", "davs", 443 },
    { "_webdav._tcp", "dav", 80 },
    { "_ftp._tcp", "ftp", 21 },
};

typedef struct
{
    char *server;
    char *domain;
    const ServiceKind *kind;
    /* Bonjour reports a service once for each network interface */
    guint n_interfaces;

    /* NULL until resolved */
    char *uri;

    DNSServiceRef resolve_ref;
    guint resolve_watch;
} Service;

/* Services are changed in the main thread, but read from the threads
 * GIO enumerates directories in. */
static GMutex services_lock;
static GPtrArray *services;

/* Main thread only */
static GList *network_monitors;

static void
service_stop_resolving (Service *service)
{
    g_clear_handle_id (&service->resolve_watch, g_source_remove);
    if (service->resolve_ref != NULL)
    {
        DNSServiceRefDeallocate (service->resolve_ref);
        service->resolve_ref = NULL;
    }
}

static void
service_free (Service *service)
{
    service_stop_resolving (service);
    g_free (service->server);
    g_free (service->domain);
    g_free (service->uri);
    g_free (service);
}

/* A name that can be used as a file name */
static char *
server_file_name (const char *server)
{
    return g_strdelimit (g_strdup (server), "/", '_');
}

/* Call with services_lock held */
static Service *
best_service_for_name (const char *name)
{
    Service *best = NULL;

    for (guint i = 0; i < services->len; i++)
    {
        Service *service = g_ptr_array_index (services, i);
        g_autofree char *file_name = NULL;

        if (service->uri == NULL)
        {
            continue;
        }

        file_name = server_file_name (service->server);
        if (g_str_equal (file_name, name) &&
            (best == NULL || service->kind < best->kind))
        {
            best = service;
        }
    }

    return best;
}

static char *
best_uri_for_server (const char *server)
{
    g_autofree char *name = server_file_name (server);
    Service *best;
    char *uri;

    g_mutex_lock (&services_lock);
    best = best_service_for_name (name);
    uri = best != NULL ? g_strdup (best->uri) : NULL;
    g_mutex_unlock (&services_lock);

    return uri;
}

static GFile *network_file_new (const char *scheme,
                                const char *name);

static void
emit_network_event (const char        *server,
                    GFileMonitorEvent  event)
{
    g_autofree char *name = server_file_name (server);
    g_autoptr (GFile) child = network_file_new (NETWORK_SCHEME, name);

    for (GList *l = network_monitors; l != NULL; l = l->next)
    {
        g_file_monitor_emit_event (G_FILE_MONITOR (l->data), child, NULL, event);
    }
}

/* Tells monitors how a server's entry changed, given its address before */
static void
notify_server_changed (const char *server,
                       const char *uri_before)
{
    g_autofree char *uri_after = best_uri_for_server (server);

    if (uri_before == NULL && uri_after != NULL)
    {
        emit_network_event (server, G_FILE_MONITOR_EVENT_CREATED);
    }
    else if (uri_before != NULL && uri_after == NULL)
    {
        emit_network_event (server, G_FILE_MONITOR_EVENT_DELETED);
    }
    else if (g_strcmp0 (uri_before, uri_after) != 0)
    {
        emit_network_event (server, G_FILE_MONITOR_EVENT_CHANGED);
    }
}

static char *
txt_record_get_string (const void *txt_record,
                       guint16     txt_length,
                       const char *key)
{
    guint8 length = 0;
    const void *value = TXTRecordGetValuePtr (txt_length, txt_record, key, &length);

    return value != NULL && length > 0 ? g_strndup (value, length) : NULL;
}

static void
resolve_reply (DNSServiceRef         ref,
               DNSServiceFlags       flags,
               uint32_t              interface,
               DNSServiceErrorType   error,
               const char           *full_name,
               const char           *host_target,
               uint16_t              port,
               uint16_t              txt_length,
               const unsigned char  *txt_record,
               void                 *context)
{
    Service *service = context;
    g_autofree char *host = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri_before = NULL;
    g_autofree char *escaped_path = NULL;
    GString *uri;

    if (error != kDNSServiceErr_NoError || host_target == NULL)
    {
        g_debug ("Could not resolve “%s”: error %d", service->server, error);
        return;
    }

    host = g_strdup (host_target);
    if (g_str_has_suffix (host, "."))
    {
        host[strlen (host) - 1] = '\0';
    }

    /* WebDAV and NFS servers may advertise the folder they share */
    path = txt_record_get_string (txt_record, txt_length, "path");
    escaped_path = path != NULL ? g_uri_escape_string (path[0] == '/' ? path + 1 : path,
                                                       G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
                                                       FALSE) : NULL;

    port = ntohs (port);
    uri = g_string_new (service->kind->scheme);
    g_string_append_printf (uri, "://%s", host);
    if (port != 0 && port != service->kind->default_port)
    {
        g_string_append_printf (uri, ":%u", port);
    }
    g_string_append_printf (uri, "/%s", escaped_path != NULL ? escaped_path : "");

    g_debug ("“%s” is at %s", service->server, uri->str);
    uri_before = best_uri_for_server (service->server);

    g_mutex_lock (&services_lock);
    g_free (service->uri);
    service->uri = g_string_free (uri, FALSE);
    g_mutex_unlock (&services_lock);

    notify_server_changed (service->server, uri_before);
}

static gboolean
on_resolve_ready (int          fd,
                  GIOCondition condition,
                  gpointer     user_data)
{
    Service *service = user_data;

    if (DNSServiceProcessResult (service->resolve_ref) != kDNSServiceErr_NoError ||
        service->uri != NULL)
    {
        /* The address is all that's needed */
        service->resolve_watch = 0;
        DNSServiceRefDeallocate (service->resolve_ref);
        service->resolve_ref = NULL;

        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
service_start_resolving (Service  *service,
                         uint32_t  interface)
{
    if (DNSServiceResolve (&service->resolve_ref, 0, interface,
                           service->server, service->kind->service_type, service->domain,
                           resolve_reply, service) != kDNSServiceErr_NoError)
    {
        service->resolve_ref = NULL;
        return;
    }

    service->resolve_watch = g_unix_fd_add (DNSServiceRefSockFD (service->resolve_ref),
                                            G_IO_IN, on_resolve_ready, service);
}

/* Call with services_lock held */
static Service *
find_service (const char        *server,
              const char        *domain,
              const ServiceKind *kind)
{
    for (guint i = 0; i < services->len; i++)
    {
        Service *service = g_ptr_array_index (services, i);

        if (service->kind == kind &&
            g_str_equal (service->server, server) &&
            g_str_equal (service->domain, domain))
        {
            return service;
        }
    }

    return NULL;
}

static void
browse_reply (DNSServiceRef        ref,
              DNSServiceFlags      flags,
              uint32_t             interface,
              DNSServiceErrorType  error,
              const char          *server,
              const char          *service_type,
              const char          *domain,
              void                *context)
{
    const ServiceKind *kind = context;
    g_autofree char *uri_before = NULL;
    Service *service;

    if (error != kDNSServiceErr_NoError)
    {
        /* kDNSServiceErr_PolicyDenied: the user didn't allow access to
         * the local network */
        g_debug ("Looking for %s servers failed: error %d", kind->scheme, error);
        return;
    }

    g_debug ("%s %s server “%s”", (flags & kDNSServiceFlagsAdd) ? "Found" : "Lost",
             kind->scheme, server);
    uri_before = best_uri_for_server (server);

    g_mutex_lock (&services_lock);
    service = find_service (server, domain, kind);
    if (flags & kDNSServiceFlagsAdd)
    {
        if (service != NULL)
        {
            service->n_interfaces++;
        }
        else
        {
            service = g_new0 (Service, 1);
            service->server = g_strdup (server);
            service->domain = g_strdup (domain);
            service->kind = kind;
            service->n_interfaces = 1;
            g_ptr_array_add (services, service);

            service_start_resolving (service, interface);
        }
    }
    else if (service != NULL && --service->n_interfaces == 0)
    {
        /* Frees it */
        g_ptr_array_remove (services, service);
    }
    g_mutex_unlock (&services_lock);

    notify_server_changed (server, uri_before);
}

static gboolean
on_browse_ready (int          fd,
                 GIOCondition condition,
                 gpointer     user_data)
{
    DNSServiceRef ref = user_data;

    if (DNSServiceProcessResult (ref) != kDNSServiceErr_NoError)
    {
        g_warning ("Stopped looking for servers on the network");
        DNSServiceRefDeallocate (ref);

        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static gboolean
start_browsing_in_main_thread (gpointer user_data)
{
    for (guint i = 0; i < G_N_ELEMENTS (service_kinds); i++)
    {
        DNSServiceRef ref = NULL;
        DNSServiceErrorType error;

        error = DNSServiceBrowse (&ref, 0, kDNSServiceInterfaceIndexAny,
                                  service_kinds[i].service_type, NULL,
                                  browse_reply, (void *) &service_kinds[i]);
        if (error != kDNSServiceErr_NoError)
        {
            g_warning ("Could not look for %s servers: error %d",
                       service_kinds[i].scheme, error);
            continue;
        }

        g_unix_fd_add (DNSServiceRefSockFD (ref), G_IO_IN, on_browse_ready, ref);
        g_debug ("Looking for %s servers", service_kinds[i].scheme);
    }

    return G_SOURCE_REMOVE;
}

/* Browsing starts when the Network view is first opened rather than at
 * startup, as macOS asks the user before apps can use the local network. */
static void
ensure_browsing (void)
{
    static gsize started = 0;

    if (g_once_init_enter (&started))
    {
        g_main_context_invoke (NULL, start_browsing_in_main_thread, NULL);
        g_once_init_leave (&started, 1);
    }
}

static GPtrArray *
list_servers (void)
{
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);

    g_mutex_lock (&services_lock);
    for (guint i = 0; i < services->len; i++)
    {
        Service *service = g_ptr_array_index (services, i);
        g_autofree char *name = NULL;

        if (service->uri == NULL)
        {
            continue;
        }

        name = server_file_name (service->server);
        if (!g_ptr_array_find_with_equal_func (names, name, g_str_equal, NULL))
        {
            g_ptr_array_add (names, g_steal_pointer (&name));
        }
    }
    g_mutex_unlock (&services_lock);

    return names;
}

/* ------------------------------------------------------------------------ */
/* File infos                                                               */
/* ------------------------------------------------------------------------ */

static void
set_read_only (GFileInfo *info)
{
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
}

static void
set_icons (GFileInfo  *info,
           const char *icon_name)
{
    g_autofree char *symbolic_name = g_strconcat (icon_name, "-symbolic", NULL);
    g_autoptr (GIcon) icon = g_themed_icon_new (icon_name);
    g_autoptr (GIcon) symbolic_icon = g_themed_icon_new (symbolic_name);

    g_file_info_set_icon (info, icon);
    g_file_info_set_symbolic_icon (info, symbolic_icon);
}

static GFileInfo *
root_info (const char *scheme)
{
    GFileInfo *info = g_file_info_new ();
    gboolean is_network = g_str_equal (scheme, NETWORK_SCHEME);

    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, is_network ? _("Network") : _("Computer"));
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
    set_icons (info, is_network ? "network-workgroup" : "computer");
    set_read_only (info);

    return info;
}

static GFileInfo *
server_info (const char *name)
{
    g_autofree char *server = NULL;
    g_autofree char *uri = NULL;
    GFileInfo *info;
    Service *best;

    g_mutex_lock (&services_lock);
    best = best_service_for_name (name);
    if (best != NULL)
    {
        server = g_strdup (best->server);
        uri = g_strdup (best->uri);
    }
    g_mutex_unlock (&services_lock);

    if (uri == NULL)
    {
        return NULL;
    }

    info = g_file_info_new ();
    g_file_info_set_name (info, name);
    g_file_info_set_display_name (info, server);
    g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
    /* Lets Nautilus open it as a folder, which mounts it */
    g_file_info_set_content_type (info, "inode/directory");
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, uri);
    set_icons (info, "network-server");
    set_read_only (info);

    return info;
}

static char *
mount_file_name (GMount *mount)
{
    g_autoptr (GFile) root = g_mount_get_root (mount);
    g_autofree char *basename = g_file_get_basename (root);

    return server_file_name (basename);
}

static GMount *
find_mount_by_file_name (const char *name)
{
    g_autoptr (GVolumeMonitor) monitor = g_volume_monitor_get ();
    g_autolist (GMount) mounts = g_volume_monitor_get_mounts (monitor);

    for (GList *l = mounts; l != NULL; l = l->next)
    {
        g_autofree char *file_name = mount_file_name (l->data);

        if (g_str_equal (file_name, name))
        {
            return g_object_ref (l->data);
        }
    }

    return NULL;
}

static GFileInfo *
mount_info (GMount *mount)
{
    GFileInfo *info = g_file_info_new ();
    g_autofree char *name = mount_file_name (mount);
    g_autofree char *display_name = g_mount_get_name (mount);
    g_autoptr (GFile) root = g_mount_get_root (mount);
    g_autofree char *root_uri = g_file_get_uri (root);
    g_autoptr (GIcon) icon = g_mount_get_icon (mount);
    g_autoptr (GIcon) symbolic_icon = g_mount_get_symbolic_icon (mount);

    g_file_info_set_name (info, name);
    g_file_info_set_display_name (info, display_name);
    g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, root_uri);
    g_file_info_set_icon (info, icon);
    g_file_info_set_symbolic_icon (info, symbolic_icon);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT,
                                       g_mount_can_unmount (mount));
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_EJECT,
                                       g_mount_can_eject (mount));
    set_read_only (info);

    return info;
}

/* ------------------------------------------------------------------------ */
/* GFile                                                                    */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_NETWORK_FILE (nautilus_macos_network_file_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosNetworkFile, nautilus_macos_network_file,
                      NAUTILUS, MACOS_NETWORK_FILE, GObject)

struct _NautilusMacosNetworkFile
{
    GObject parent_instance;

    /* NETWORK_SCHEME or COMPUTER_SCHEME */
    const char *scheme;
    /* "" for the root, otherwise the name of an item in it */
    char *name;
};

static void file_iface_init (GFileIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (NautilusMacosNetworkFile, nautilus_macos_network_file, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_FILE, file_iface_init))

static void
nautilus_macos_network_file_finalize (GObject *object)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (object);

    g_free (self->name);

    G_OBJECT_CLASS (nautilus_macos_network_file_parent_class)->finalize (object);
}

static void
nautilus_macos_network_file_class_init (NautilusMacosNetworkFileClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = nautilus_macos_network_file_finalize;
}

static void
nautilus_macos_network_file_init (NautilusMacosNetworkFile *self)
{
}

static GFile *
network_file_new (const char *scheme,
                  const char *name)
{
    NautilusMacosNetworkFile *self = g_object_new (NAUTILUS_TYPE_MACOS_NETWORK_FILE, NULL);
    const char *start = name != NULL ? name : "";
    gsize length;

    self->scheme = g_str_equal (scheme, NETWORK_SCHEME) ? NETWORK_SCHEME : COMPUTER_SCHEME;

    while (*start == '/')
    {
        start++;
    }
    length = strlen (start);
    while (length > 0 && start[length - 1] == '/')
    {
        length--;
    }
    self->name = g_strndup (start, length);

    return G_FILE (self);
}

static gboolean
is_root (NautilusMacosNetworkFile *self)
{
    return self->name[0] == '\0';
}

static gboolean
is_network (NautilusMacosNetworkFile *self)
{
    return g_str_equal (self->scheme, NETWORK_SCHEME);
}

static GFile *
network_file_dup (GFile *file)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);

    return network_file_new (self->scheme, self->name);
}

static guint
network_file_hash (GFile *file)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);

    return g_str_hash (self->scheme) ^ g_str_hash (self->name);
}

static gboolean
network_file_equal (GFile *file1,
                    GFile *file2)
{
    NautilusMacosNetworkFile *self1 = NAUTILUS_MACOS_NETWORK_FILE (file1);

    return NAUTILUS_IS_MACOS_NETWORK_FILE (file2) &&
           g_str_equal (self1->scheme, NAUTILUS_MACOS_NETWORK_FILE (file2)->scheme) &&
           g_str_equal (self1->name, NAUTILUS_MACOS_NETWORK_FILE (file2)->name);
}

static gboolean
network_file_is_native (GFile *file)
{
    return FALSE;
}

static gboolean
network_file_has_uri_scheme (GFile      *file,
                             const char *uri_scheme)
{
    return g_ascii_strcasecmp (uri_scheme, NAUTILUS_MACOS_NETWORK_FILE (file)->scheme) == 0;
}

static char *
network_file_get_uri_scheme (GFile *file)
{
    return g_strdup (NAUTILUS_MACOS_NETWORK_FILE (file)->scheme);
}

static char *
network_file_get_basename (GFile *file)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);

    return g_strdup (is_root (self) ? "/" : self->name);
}

static char *
network_file_get_path (GFile *file)
{
    return NULL;
}

static char *
network_file_get_uri (GFile *file)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);
    g_autofree char *escaped = g_uri_escape_string (self->name, NULL, FALSE);

    return g_strconcat (self->scheme, ":///", escaped, NULL);
}

static GFile *
network_file_get_parent (GFile *file)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);

    return is_root (self) ? NULL : network_file_new (self->scheme, "");
}

static gboolean
network_file_prefix_matches (GFile *prefix,
                             GFile *file)
{
    return NAUTILUS_IS_MACOS_NETWORK_FILE (prefix) &&
           NAUTILUS_IS_MACOS_NETWORK_FILE (file) &&
           g_str_equal (NAUTILUS_MACOS_NETWORK_FILE (prefix)->scheme,
                        NAUTILUS_MACOS_NETWORK_FILE (file)->scheme) &&
           is_root (NAUTILUS_MACOS_NETWORK_FILE (prefix)) &&
           !is_root (NAUTILUS_MACOS_NETWORK_FILE (file));
}

static char *
network_file_get_relative_path (GFile *parent,
                                GFile *descendant)
{
    if (!network_file_prefix_matches (parent, descendant))
    {
        return NULL;
    }

    return g_strdup (NAUTILUS_MACOS_NETWORK_FILE (descendant)->name);
}

static GFile *
network_file_resolve_relative_path (GFile      *file,
                                    const char *relative_path)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);
    g_autofree char *joined = NULL;

    if (g_path_is_absolute (relative_path) || is_root (self))
    {
        return network_file_new (self->scheme, relative_path);
    }

    /* Items have no children; the result just won't exist */
    joined = g_build_filename (self->name, relative_path, NULL);

    return network_file_new (self->scheme, joined);
}

static GFile *
network_file_get_child_for_display_name (GFile       *file,
                                         const char  *display_name,
                                         GError     **error)
{
    return network_file_resolve_relative_path (file, display_name);
}

static GFileInfo *
network_file_query_info (GFile                *file,
                         const char           *attributes,
                         GFileQueryInfoFlags   flags,
                         GCancellable         *cancellable,
                         GError              **error)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);
    GFileInfo *info = NULL;

    if (is_root (self))
    {
        return root_info (self->scheme);
    }

    if (is_network (self))
    {
        info = server_info (self->name);
    }
    else
    {
        g_autoptr (GMount) mount = find_mount_by_file_name (self->name);

        info = mount != NULL ? mount_info (mount) : NULL;
    }

    if (info == NULL)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             _("File not found"));
    }

    return info;
}

static GFileInfo *
network_file_query_filesystem_info (GFile         *file,
                                    const char    *attributes,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
    GFileInfo *info = g_file_info_new ();

    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE,
                                       is_network (NAUTILUS_MACOS_NETWORK_FILE (file)));

    return info;
}

/* Nautilus mounts network:/// before showing it, which GVfs needs */
static void
network_file_mount_enclosing_volume (GFile               *file,
                                     GMountMountFlags     flags,
                                     GMountOperation     *mount_operation,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
    g_autoptr (GTask) task = g_task_new (file, cancellable, callback, user_data);

    if (is_network (NAUTILUS_MACOS_NETWORK_FILE (file)))
    {
        ensure_browsing ();
    }

    g_task_return_boolean (task, TRUE);
}

static gboolean
network_file_mount_enclosing_volume_finish (GFile         *file,
                                            GAsyncResult  *result,
                                            GError       **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

static void
mount_operation_done (GObject      *source,
                      GAsyncResult *result,
                      gpointer      user_data)
{
    g_autoptr (GTask) task = user_data;
    gboolean ejecting = GPOINTER_TO_INT (g_task_get_task_data (task));
    GError *error = NULL;
    gboolean done;

    done = ejecting ? g_mount_eject_with_operation_finish (G_MOUNT (source), result, &error)
                    : g_mount_unmount_with_operation_finish (G_MOUNT (source), result, &error);
    if (done)
    {
        g_task_return_boolean (task, TRUE);
    }
    else
    {
        g_task_return_error (task, error);
    }
}

/* Unmounting or ejecting an item of computer:/// does it to its volume */
static void
run_mount_operation (GFile               *file,
                     gboolean             eject,
                     GMountUnmountFlags   flags,
                     GMountOperation     *mount_operation,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);
    GTask *task = g_task_new (file, cancellable, callback, user_data);
    g_autoptr (GMount) mount = NULL;

    g_task_set_task_data (task, GINT_TO_POINTER (eject), NULL);

    if (!is_network (self) && !is_root (self))
    {
        mount = find_mount_by_file_name (self->name);
    }

    if (mount == NULL)
    {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
                                 _("File isn’t mounted"));
        g_object_unref (task);
        return;
    }

    if (eject)
    {
        g_mount_eject_with_operation (mount, flags, mount_operation, cancellable,
                                      mount_operation_done, task);
    }
    else
    {
        g_mount_unmount_with_operation (mount, flags, mount_operation, cancellable,
                                        mount_operation_done, task);
    }
}

static void
network_file_unmount_mountable (GFile               *file,
                                GMountUnmountFlags   flags,
                                GMountOperation     *mount_operation,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
    run_mount_operation (file, FALSE, flags, mount_operation, cancellable, callback, user_data);
}

static void
network_file_eject_mountable (GFile               *file,
                              GMountUnmountFlags   flags,
                              GMountOperation     *mount_operation,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    run_mount_operation (file, TRUE, flags, mount_operation, cancellable, callback, user_data);
}

static gboolean
network_file_mount_operation_finish (GFile         *file,
                                     GAsyncResult  *result,
                                     GError       **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------------ */
/* Enumerator                                                               */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_NETWORK_ENUMERATOR (nautilus_macos_network_enumerator_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosNetworkEnumerator, nautilus_macos_network_enumerator,
                      NAUTILUS, MACOS_NETWORK_ENUMERATOR, GFileEnumerator)

struct _NautilusMacosNetworkEnumerator
{
    GFileEnumerator parent_instance;

    GList *infos;
};

G_DEFINE_FINAL_TYPE (NautilusMacosNetworkEnumerator, nautilus_macos_network_enumerator,
                     G_TYPE_FILE_ENUMERATOR)

static GFileInfo *
network_enumerator_next_file (GFileEnumerator  *enumerator,
                              GCancellable     *cancellable,
                              GError          **error)
{
    NautilusMacosNetworkEnumerator *self = NAUTILUS_MACOS_NETWORK_ENUMERATOR (enumerator);
    GFileInfo *info;

    if (self->infos == NULL)
    {
        return NULL;
    }

    info = self->infos->data;
    self->infos = g_list_delete_link (self->infos, self->infos);

    return info;
}

static gboolean
network_enumerator_close (GFileEnumerator  *enumerator,
                          GCancellable     *cancellable,
                          GError          **error)
{
    return TRUE;
}

static void
nautilus_macos_network_enumerator_finalize (GObject *object)
{
    NautilusMacosNetworkEnumerator *self = NAUTILUS_MACOS_NETWORK_ENUMERATOR (object);

    g_list_free_full (self->infos, g_object_unref);

    G_OBJECT_CLASS (nautilus_macos_network_enumerator_parent_class)->finalize (object);
}

static void
nautilus_macos_network_enumerator_class_init (NautilusMacosNetworkEnumeratorClass *klass)
{
    GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);

    G_OBJECT_CLASS (klass)->finalize = nautilus_macos_network_enumerator_finalize;
    enumerator_class->next_file = network_enumerator_next_file;
    enumerator_class->close_fn = network_enumerator_close;
}

static void
nautilus_macos_network_enumerator_init (NautilusMacosNetworkEnumerator *self)
{
}

static GFileEnumerator *
network_file_enumerate_children (GFile                *file,
                                 const char           *attributes,
                                 GFileQueryInfoFlags   flags,
                                 GCancellable         *cancellable,
                                 GError              **error)
{
    NautilusMacosNetworkFile *self = NAUTILUS_MACOS_NETWORK_FILE (file);
    NautilusMacosNetworkEnumerator *enumerator;
    GList *infos = NULL;

    if (!is_root (self))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
                             _("The file is not a directory"));
        return NULL;
    }

    if (is_network (self))
    {
        g_autoptr (GPtrArray) names = NULL;

        ensure_browsing ();

        names = list_servers ();
        for (guint i = 0; i < names->len; i++)
        {
            GFileInfo *info = server_info (g_ptr_array_index (names, i));

            if (info != NULL)
            {
                infos = g_list_prepend (infos, info);
            }
        }
    }
    else
    {
        g_autoptr (GVolumeMonitor) monitor = g_volume_monitor_get ();
        g_autolist (GMount) mounts = g_volume_monitor_get_mounts (monitor);

        for (GList *l = mounts; l != NULL; l = l->next)
        {
            infos = g_list_prepend (infos, mount_info (l->data));
        }
    }

    enumerator = g_object_new (NAUTILUS_TYPE_MACOS_NETWORK_ENUMERATOR,
                               "container", file,
                               NULL);
    enumerator->infos = g_list_reverse (infos);

    return G_FILE_ENUMERATOR (enumerator);
}

/* ------------------------------------------------------------------------ */
/* Monitor                                                                  */
/* ------------------------------------------------------------------------ */

#define NAUTILUS_TYPE_MACOS_NETWORK_MONITOR (nautilus_macos_network_monitor_get_type ())
G_DECLARE_FINAL_TYPE (NautilusMacosNetworkMonitor, nautilus_macos_network_monitor,
                      NAUTILUS, MACOS_NETWORK_MONITOR, GFileMonitor)

struct _NautilusMacosNetworkMonitor
{
    GFileMonitor parent_instance;

    /* For computer:/// */
    GVolumeMonitor *volume_monitor;
};

G_DEFINE_FINAL_TYPE (NautilusMacosNetworkMonitor, nautilus_macos_network_monitor, G_TYPE_FILE_MONITOR)

static void
emit_mount_event (NautilusMacosNetworkMonitor *self,
                  GMount                      *mount,
                  GFileMonitorEvent            event)
{
    g_autofree char *name = mount_file_name (mount);
    g_autoptr (GFile) child = network_file_new (COMPUTER_SCHEME, name);

    g_file_monitor_emit_event (G_FILE_MONITOR (self), child, NULL, event);
}

static void
on_mount_added (GVolumeMonitor              *volume_monitor,
                GMount                      *mount,
                NautilusMacosNetworkMonitor *self)
{
    emit_mount_event (self, mount, G_FILE_MONITOR_EVENT_CREATED);
}

static void
on_mount_removed (GVolumeMonitor              *volume_monitor,
                  GMount                      *mount,
                  NautilusMacosNetworkMonitor *self)
{
    emit_mount_event (self, mount, G_FILE_MONITOR_EVENT_DELETED);
}

static void
on_mount_changed (GVolumeMonitor              *volume_monitor,
                  GMount                      *mount,
                  NautilusMacosNetworkMonitor *self)
{
    emit_mount_event (self, mount, G_FILE_MONITOR_EVENT_CHANGED);
}

static void
network_monitor_stop (NautilusMacosNetworkMonitor *self)
{
    network_monitors = g_list_remove (network_monitors, self);

    if (self->volume_monitor != NULL)
    {
        g_signal_handlers_disconnect_by_data (self->volume_monitor, self);
        g_clear_object (&self->volume_monitor);
    }
}

static gboolean
network_monitor_cancel (GFileMonitor *monitor)
{
    network_monitor_stop (NAUTILUS_MACOS_NETWORK_MONITOR (monitor));

    return TRUE;
}

static void
nautilus_macos_network_monitor_dispose (GObject *object)
{
    network_monitor_stop (NAUTILUS_MACOS_NETWORK_MONITOR (object));

    G_OBJECT_CLASS (nautilus_macos_network_monitor_parent_class)->dispose (object);
}

static void
nautilus_macos_network_monitor_class_init (NautilusMacosNetworkMonitorClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = nautilus_macos_network_monitor_dispose;
    G_FILE_MONITOR_CLASS (klass)->cancel = network_monitor_cancel;
}

static void
nautilus_macos_network_monitor_init (NautilusMacosNetworkMonitor *self)
{
}

static GFileMonitor *
network_file_monitor (GFile              *file,
                      GFileMonitorFlags   flags,
                      GCancellable       *cancellable,
                      GError            **error)
{
    NautilusMacosNetworkFile *network_file = NAUTILUS_MACOS_NETWORK_FILE (file);
    NautilusMacosNetworkMonitor *self = g_object_new (NAUTILUS_TYPE_MACOS_NETWORK_MONITOR, NULL);

    /* Only the roots change; monitors of their items report nothing */
    if (!is_root (network_file))
    {
        return G_FILE_MONITOR (self);
    }

    if (is_network (network_file))
    {
        /* Monitors are created in the main thread, where servers change */
        network_monitors = g_list_prepend (network_monitors, self);
        ensure_browsing ();
    }
    else
    {
        self->volume_monitor = g_volume_monitor_get ();
        g_signal_connect (self->volume_monitor, "mount-added", G_CALLBACK (on_mount_added), self);
        g_signal_connect (self->volume_monitor, "mount-removed", G_CALLBACK (on_mount_removed), self);
        g_signal_connect (self->volume_monitor, "mount-changed", G_CALLBACK (on_mount_changed), self);
    }

    return G_FILE_MONITOR (self);
}

static void
file_iface_init (GFileIface *iface)
{
    iface->dup = network_file_dup;
    iface->hash = network_file_hash;
    iface->equal = network_file_equal;
    iface->is_native = network_file_is_native;
    iface->has_uri_scheme = network_file_has_uri_scheme;
    iface->get_uri_scheme = network_file_get_uri_scheme;
    iface->get_basename = network_file_get_basename;
    iface->get_path = network_file_get_path;
    iface->get_uri = network_file_get_uri;
    iface->get_parse_name = network_file_get_uri;
    iface->get_parent = network_file_get_parent;
    iface->prefix_matches = network_file_prefix_matches;
    iface->get_relative_path = network_file_get_relative_path;
    iface->resolve_relative_path = network_file_resolve_relative_path;
    iface->get_child_for_display_name = network_file_get_child_for_display_name;
    iface->enumerate_children = network_file_enumerate_children;
    iface->query_info = network_file_query_info;
    iface->query_filesystem_info = network_file_query_filesystem_info;
    iface->mount_enclosing_volume = network_file_mount_enclosing_volume;
    iface->mount_enclosing_volume_finish = network_file_mount_enclosing_volume_finish;
    iface->unmount_mountable_with_operation = network_file_unmount_mountable;
    iface->unmount_mountable_with_operation_finish = network_file_mount_operation_finish;
    iface->eject_mountable_with_operation = network_file_eject_mountable;
    iface->eject_mountable_with_operation_finish = network_file_mount_operation_finish;
    iface->monitor_dir = network_file_monitor;
    iface->monitor_file = network_file_monitor;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

/* Also called for every parse name, whatever its scheme: only handle ours,
 * returning NULL lets GVfs handle the others. */
static GFile *
lookup_uri (GVfs       *vfs,
            const char *identifier,
            gpointer    user_data)
{
    const char *scheme = user_data;
    gsize prefix_length = strlen (scheme) + 1;
    g_autofree char *name = NULL;

    if (g_ascii_strncasecmp (identifier, scheme, prefix_length - 1) != 0 ||
        identifier[prefix_length - 1] != ':')
    {
        return NULL;
    }

    name = g_uri_unescape_string (identifier + prefix_length, NULL);
    if (name == NULL)
    {
        return NULL;
    }

    return network_file_new (scheme, name);
}

/**
 * nautilus_macos_network_register:
 *
 * Provides network:// and computer:// unless GVfs already does, which
 * shows Nautilus's Network view.
 */
void
nautilus_macos_network_register (void)
{
    static const char * const schemes[] = { NETWORK_SCHEME, COMPUTER_SCHEME };
    GVfs *vfs = g_vfs_get_default ();

    services = g_ptr_array_new_with_free_func ((GDestroyNotify) service_free);

    for (guint i = 0; i < G_N_ELEMENTS (schemes); i++)
    {
        if (g_strv_contains (g_vfs_get_supported_uri_schemes (vfs), schemes[i]))
        {
            continue;
        }

        g_vfs_register_uri_scheme (vfs, schemes[i],
                                   lookup_uri, (gpointer) schemes[i], NULL,
                                   lookup_uri, (gpointer) schemes[i], NULL);
    }
}
