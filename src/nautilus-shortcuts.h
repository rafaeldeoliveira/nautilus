/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Modifier for keyboard shortcuts and modifier-clicks: Command on macOS,
 * Control everywhere else. GTK parses <Primary> as Control on every
 * platform, so accelerator strings go through nautilus_shortcut_for_platform().
 */
#ifdef __APPLE__
# define NAUTILUS_PRIMARY_MASK GDK_META_MASK
#else
# define NAUTILUS_PRIMARY_MASK GDK_CONTROL_MASK
#endif

char               *nautilus_shortcut_for_platform  (const char *trigger);
GtkShortcutTrigger *nautilus_shortcut_trigger_parse (const char *trigger);

G_END_DECLS
