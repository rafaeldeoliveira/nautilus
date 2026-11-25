/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/* macOS integration that needs Apple's frameworks:
 *
 * - Packages: directories that macOS presents as single items, such as
 *   applications (.app) and some documents (.rtfd, .pages…). Finder opens
 *   them instead of showing their contents, and so does Nautilus.
 * - Opening items as Finder does, through Launch Services.
 * - Thumbnails and application icons from Quick Look.
 * - Network shares (SMB, AFP, NFS, WebDAV, FTP), mounted by macOS under
 *   /Volumes as in Finder's "Connect to Server", with its login dialog
 *   and Keychain.
 */

#define G_LOG_DOMAIN "nautilus-macos"

#import <AppKit/AppKit.h>
#import <NetFS/NetFS.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <glib/gi18n.h>

#include "nautilus-macos.h"

/* Quick Look normally answers within milliseconds; don't block a
 * thumbnailing thread forever on a file it chokes on. */
#define THUMBNAIL_TIMEOUT_SECONDS 20

gboolean
nautilus_macos_path_is_package (const char *path)
{
    @autoreleasepool
    {
        NSURL *url = [NSURL fileURLWithPath:@(path)];
        NSNumber *is_package = nil;

        if (![url getResourceValue:&is_package forKey:NSURLIsPackageKey error:nil])
        {
            return FALSE;
        }

        return is_package.boolValue;
    }
}

gboolean
nautilus_macos_location_is_package (GFile *location)
{
    g_autofree char *path = location != NULL ? g_file_get_path (location) : NULL;

    return path != NULL && nautilus_macos_path_is_package (path);
}

/**
 * nautilus_macos_open_location:
 *
 * Opens @location as Finder does: launches applications, and opens other
 * items with the application macOS associates with them.
 */
gboolean
nautilus_macos_open_location (GFile *location)
{
    g_autofree char *path = g_file_get_path (location);

    if (path == NULL)
    {
        return FALSE;
    }

    @autoreleasepool
    {
        return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:@(path)]];
    }
}

static void
free_pixels (guchar   *pixels,
             gpointer  data)
{
    g_free (pixels);
}

static GdkPixbuf *
pixbuf_from_cg_image (CGImageRef image)
{
    size_t width = CGImageGetWidth (image);
    size_t height = CGImageGetHeight (image);
    size_t stride = width * 4;
    guchar *pixels = g_malloc0 (stride * height);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB ();
    CGContextRef context;

    context = CGBitmapContextCreate (pixels, width, height, 8, stride, color_space,
                                     kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease (color_space);

    if (context == NULL)
    {
        g_free (pixels);
        return NULL;
    }

    CGContextDrawImage (context, CGRectMake (0, 0, width, height), image);
    CGContextRelease (context);

    /* GdkPixbuf expects straight (non-premultiplied) alpha */
    for (size_t i = 0; i < stride * height; i += 4)
    {
        guchar alpha = pixels[i + 3];

        if (alpha != 0 && alpha != 255)
        {
            pixels[i] = MIN (255, pixels[i] * 255 / alpha);
            pixels[i + 1] = MIN (255, pixels[i + 1] * 255 / alpha);
            pixels[i + 2] = MIN (255, pixels[i + 2] * 255 / alpha);
        }
    }

    return gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, TRUE, 8,
                                     width, height, stride, free_pixels, NULL);
}

/**
 * nautilus_macos_create_thumbnail:
 * @path: a local file
 * @size: the maximum width and height, in pixels
 * @allow_icon: whether the item's icon is wanted when there is no
 *   thumbnail (as for applications)
 *
 * Blocks until Quick Look answers: call it from a thread.
 *
 * Returns: (transfer full) (nullable): the thumbnail
 */
GdkPixbuf *
nautilus_macos_create_thumbnail (const char *path,
                                 int         size,
                                 gboolean    allow_icon)
{
    @autoreleasepool
    {
        QLThumbnailGenerationRequestRepresentationTypes types =
            QLThumbnailGenerationRequestRepresentationTypeLowQualityThumbnail |
            QLThumbnailGenerationRequestRepresentationTypeThumbnail;
        QLThumbnailGenerationRequest *request;
        dispatch_semaphore_t done = dispatch_semaphore_create (0);
        __block CGImageRef image = NULL;
        GdkPixbuf *pixbuf;

        if (allow_icon)
        {
            types |= QLThumbnailGenerationRequestRepresentationTypeIcon;
        }

        request = [[QLThumbnailGenerationRequest alloc] initWithFileAtURL:[NSURL fileURLWithPath:@(path)]
                                                                     size:CGSizeMake (size, size)
                                                                    scale:1.0
                                                      representationTypes:types];

        [[QLThumbnailGenerator sharedGenerator]
         generateBestRepresentationForRequest:request
                            completionHandler:^(QLThumbnailRepresentation *representation,
                                                NSError                   *error) {
             if (representation != nil && representation.CGImage != NULL)
             {
                 image = CGImageRetain (representation.CGImage);
             }
             dispatch_semaphore_signal (done);
         }];

        if (dispatch_semaphore_wait (done, dispatch_time (DISPATCH_TIME_NOW,
                                                          THUMBNAIL_TIMEOUT_SECONDS * NSEC_PER_SEC)) != 0)
        {
            [[QLThumbnailGenerator sharedGenerator] cancelRequest:request];
            g_debug ("Quick Look timed out on %s", path);
            return NULL;
        }

        if (image == NULL)
        {
            return NULL;
        }

        pixbuf = pixbuf_from_cg_image (image);
        CGImageRelease (image);

        return pixbuf;
    }
}

/* Network locations macOS can mount, as in Finder's "Connect to Server".
 * dav:// and davs:// are GVfs's names for WebDAV over http(s). */
static const char * const network_schemes[] = {
    "smb", "cifs", "afp", "nfs", "ftp", "dav", "davs", "http", "https", NULL
};

gboolean
nautilus_macos_is_network_scheme (const char *scheme)
{
    return scheme != NULL && g_strv_contains (network_schemes, scheme);
}

gboolean
nautilus_macos_is_network_location (GFile *location)
{
    g_autofree char *scheme = g_file_get_uri_scheme (location);

    return nautilus_macos_is_network_scheme (scheme);
}

/* An unmounted network address. GLib's local VFS gives these "dummy"
 * files that only remember their URI, which is all that's needed to
 * mount them; going through the class bypasses our own registration. */
static GFile *
lookup_network_uri (GVfs       *vfs,
                    const char *identifier,
                    gpointer    user_data)
{
    g_autofree char *scheme = g_uri_parse_scheme (identifier);

    if (!nautilus_macos_is_network_scheme (scheme))
    {
        return NULL;
    }

    return G_VFS_GET_CLASS (vfs)->get_file_for_uri (vfs, identifier);
}

/**
 * nautilus_macos_register_network_schemes:
 *
 * Without GVfs, GLib parses a typed "smb://server/share" as a relative file
 * name. Registering the network schemes keeps them addresses, which
 * nautilus_macos_mount_network_location() can then mount.
 */
void
nautilus_macos_register_network_schemes (void)
{
    GVfs *vfs = g_vfs_get_default ();

    for (guint i = 0; network_schemes[i] != NULL; i++)
    {
        if (!g_strv_contains (g_vfs_get_supported_uri_schemes (vfs), network_schemes[i]))
        {
            g_vfs_register_uri_scheme (vfs, network_schemes[i],
                                       lookup_network_uri, NULL, NULL,
                                       lookup_network_uri, NULL, NULL);
        }
    }
}

static NSURL *
netfs_url_for_uri (const char *uri)
{
    NSString *string = @(uri);

    if ([string hasPrefix:@"dav://"])
    {
        string = [@"http://" stringByAppendingString:[string substringFromIndex:6]];
    }
    else if ([string hasPrefix:@"davs://"])
    {
        string = [@"https://" stringByAppendingString:[string substringFromIndex:7]];
    }

    return [NSURL URLWithString:string];
}

/**
 * nautilus_macos_mount_network_location:
 *
 * Mounts a network share with NetFS, which shows macOS's own login and
 * share selection dialogs, and keeps passwords in the Keychain. The
 * result is the folder it was mounted at (under /Volumes).
 */
void
nautilus_macos_mount_network_location (GFile               *location,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    GTask *task = g_task_new (NULL, cancellable, callback, user_data);
    g_autofree char *uri = g_file_get_uri (location);

    g_task_set_source_tag (task, nautilus_macos_mount_network_location);

    @autoreleasepool
    {
        NSURL *url = netfs_url_for_uri (uri);
        NSMutableDictionary *open_options = [NSMutableDictionary dictionary];
        AsyncRequestID request_id = NULL;
        char *task_uri = g_strdup (uri);
        int status;

        if (url == nil)
        {
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                     _("“%s” is not a valid server address"), uri);
            g_object_unref (task);
            g_free (task_uri);
            return;
        }

        open_options[(__bridge NSString *) kNAUIOptionKey] = (__bridge NSString *) kNAUIOptionAllowUI;

        status = NetFSMountURLAsync ((__bridge CFURLRef) url, NULL, NULL, NULL,
                                     (__bridge CFMutableDictionaryRef) open_options, NULL,
                                     &request_id,
                                     dispatch_get_global_queue (QOS_CLASS_USER_INITIATED, 0),
                                     ^(int result, AsyncRequestID request, CFArrayRef mountpoints) {
            g_autofree char *mountpoint = NULL;

            if (mountpoints != NULL && CFArrayGetCount (mountpoints) > 0)
            {
                NSString *path = (__bridge NSString *) CFArrayGetValueAtIndex (mountpoints, 0);

                mountpoint = g_strdup (path.fileSystemRepresentation);
            }

            if ((result == 0 || result == EEXIST) && mountpoint != NULL)
            {
                /* EEXIST: already mounted, which is fine */
                g_task_return_pointer (task, g_file_new_for_path (mountpoint), g_object_unref);
            }
            else if (result == ECANCELED || result == -128 /* userCanceledErr */)
            {
                /* The user closed macOS's dialog: nothing to report */
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
                                         "Cancelled");
            }
            else
            {
                g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("Could not connect to “%s”: %s"), task_uri,
                                         result > 0 ? g_strerror (result) : _("the server refused the connection"));
            }

            g_free (task_uri);
            g_object_unref (task);
        });

        if (status != 0)
        {
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     _("Could not connect to “%s”: %s"), uri, g_strerror (status));
            g_object_unref (task);
            g_free (task_uri);
        }
    }
}

GFile *
nautilus_macos_mount_network_location_finish (GAsyncResult  *result,
                                              GError       **error)
{
    return g_task_propagate_pointer (G_TASK (result), error);
}
