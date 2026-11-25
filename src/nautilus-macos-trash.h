/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Name of the extended attribute recording where a file was trashed from */
#define NAUTILUS_MACOS_TRASH_ORIGIN_XATTR "org.gnome.nautilus.trash-origin"
/* …and when (seconds since the epoch, as text) */
#define NAUTILUS_MACOS_TRASH_TIME_XATTR "org.gnome.nautilus.trash-time"

void     nautilus_macos_trash_register            (void);
gboolean nautilus_macos_trash_is_readable         (void);
void     nautilus_macos_trash_record_origin       (GFile *file);
void     nautilus_macos_trash_open_in_finder      (void);
void     nautilus_macos_trash_empty_with_finder   (void);

G_END_DECLS
