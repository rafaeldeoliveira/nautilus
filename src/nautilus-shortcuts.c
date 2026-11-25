/*
 * Copyright (C) 2026 The GNOME project contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "nautilus-shortcuts.h"

#ifdef __APPLE__
typedef struct
{
    guint keyval;
    GdkModifierType mods;
    guint mac_keyval;          /* 0: no equivalent, drop the shortcut */
    GdkModifierType mac_mods;
} MacShortcut;

/* Shortcuts that don't simply swap Control for Command, following Finder.
 * Keep in sync with build-aux/macos/macos-shortcuts.py, which rewrites the
 * accelerators shown in menus and in the shortcuts dialog. */
static const MacShortcut mac_shortcuts[] =
{
    /* Command-H hides the application */
    { GDK_KEY_h, GDK_CONTROL_MASK, GDK_KEY_period, GDK_META_MASK | GDK_SHIFT_MASK },
    /* Option-arrows move by word in text fields */
    { GDK_KEY_Left, GDK_ALT_MASK, GDK_KEY_bracketleft, GDK_META_MASK },
    { GDK_KEY_Right, GDK_ALT_MASK, GDK_KEY_bracketright, GDK_META_MASK },
    { GDK_KEY_Up, GDK_ALT_MASK, GDK_KEY_Up, GDK_META_MASK },
    { GDK_KEY_Down, GDK_ALT_MASK, GDK_KEY_Down, GDK_META_MASK },
    { GDK_KEY_Home, GDK_ALT_MASK, GDK_KEY_h, GDK_META_MASK | GDK_SHIFT_MASK },
    /* Option-digits type characters, and Command-1/2 switch the view */
    { GDK_KEY_1, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_2, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_3, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_4, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_5, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_6, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_7, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_8, GDK_ALT_MASK, 0, 0 },
    { GDK_KEY_9, GDK_ALT_MASK, 0, 0 },
};

/* Tab navigation stays on Control, as in libadwaita and Safari */
static gboolean
keeps_control (guint keyval)
{
    return keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down ||
           keyval == GDK_KEY_KP_Page_Up || keyval == GDK_KEY_KP_Page_Down ||
           keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab;
}

/* Returns NULL when the shortcut has no macOS equivalent */
static char *
accelerator_for_mac (const char *accel)
{
    guint keyval;
    GdkModifierType mods;

    if (!gtk_accelerator_parse (accel, &keyval, &mods) || keyval == 0)
    {
        /* Not a single accelerator (e.g. a named key GTK can't parse) */
        return g_strdup (accel);
    }

    for (gsize i = 0; i < G_N_ELEMENTS (mac_shortcuts); i++)
    {
        if (mac_shortcuts[i].keyval == keyval && mac_shortcuts[i].mods == mods)
        {
            if (mac_shortcuts[i].mac_keyval == 0)
            {
                return NULL;
            }

            return gtk_accelerator_name (mac_shortcuts[i].mac_keyval, mac_shortcuts[i].mac_mods);
        }
    }

    if ((mods & GDK_CONTROL_MASK) && !keeps_control (keyval))
    {
        mods = (mods & ~GDK_CONTROL_MASK) | GDK_META_MASK;
    }

    return gtk_accelerator_name (keyval, mods);
}
#endif

/**
 * nautilus_shortcut_for_platform:
 * @trigger: an accelerator, or several separated by '|' as accepted by
 *   gtk_shortcut_trigger_parse_string()
 *
 * Returns: (transfer full) (nullable): @trigger with the modifiers used
 *   on this platform, or %NULL if none of its shortcuts apply here
 */
char *
nautilus_shortcut_for_platform (const char *trigger)
{
#ifdef __APPLE__
    g_auto (GStrv) alternatives = g_strsplit (trigger, "|", -1);
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    g_auto (GStrv) result = NULL;

    for (guint i = 0; alternatives[i] != NULL; i++)
    {
        g_autofree char *accel = accelerator_for_mac (alternatives[i]);

        if (accel != NULL)
        {
            g_strv_builder_add (builder, accel);
        }
    }

    result = g_strv_builder_end (builder);

    return result[0] != NULL ? g_strjoinv ("|", result) : NULL;
#else
    return g_strdup (trigger);
#endif
}

/**
 * nautilus_shortcut_trigger_parse:
 * @trigger: see nautilus_shortcut_for_platform()
 *
 * Like gtk_shortcut_trigger_parse_string(), with the modifiers used on
 * this platform.
 *
 * Returns: (transfer full): a new trigger, which never triggers if
 *   none of the shortcuts apply here
 */
GtkShortcutTrigger *
nautilus_shortcut_trigger_parse (const char *trigger)
{
    g_autofree char *platform_trigger = nautilus_shortcut_for_platform (trigger);
    GtkShortcutTrigger *result = NULL;

    if (platform_trigger != NULL)
    {
        result = gtk_shortcut_trigger_parse_string (platform_trigger);
    }

    return result != NULL ? result : g_object_ref (gtk_never_trigger_get ());
}
