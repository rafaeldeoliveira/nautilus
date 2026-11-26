/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* macOS integration, implemented with Apple's frameworks (nautilus-macos.m) */

gboolean   nautilus_macos_location_is_package (GFile      *location);
gboolean   nautilus_macos_path_is_package     (const char *path);
gboolean   nautilus_macos_open_location       (GFile      *location);
GdkPixbuf *nautilus_macos_create_thumbnail    (const char *path,
                                               int         size,
                                               gboolean    allow_icon);

void       nautilus_macos_register_network_schemes (void);
gboolean   nautilus_macos_is_network_scheme   (const char *scheme);
gboolean   nautilus_macos_is_network_location (GFile      *location);
void       nautilus_macos_mount_network_location        (GFile                *location,
                                                         GCancellable         *cancellable,
                                                         GAsyncReadyCallback   callback,
                                                         gpointer              user_data);
GFile     *nautilus_macos_mount_network_location_finish (GAsyncResult         *result,
                                                         GError              **error);

GdkContentProvider *nautilus_macos_file_drag_content_new (GSList  *files);
void       nautilus_macos_file_drag_begin       (GdkDrag *drag);

G_END_DECLS
