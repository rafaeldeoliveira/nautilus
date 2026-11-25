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
 */

#define G_LOG_DOMAIN "nautilus-macos"

#import <AppKit/AppKit.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

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
