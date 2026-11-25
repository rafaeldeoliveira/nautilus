/*
 * Nautilus
 *
 * Copyright (C) 1999, 2000 Red Hat, Inc.
 * Copyright (C) 1999, 2000 Eazel, Inc.
 *
 * Nautilus is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nautilus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Elliot Lee <sopwith@redhat.com>,
 *          Darin Adler <darin@bentspoon.com>,
 *          John Sullivan <sullivan@eazel.com>
 *
 */

/* nautilus-main.c: Implementation of the routines that drive program lifecycle and main window creation/destruction. */

#include <config.h>

#include "nautilus-application.h"
#include "nautilus-resources.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#if __has_include (<gio/gdesktopappinfo.h>)
#include <gio/gdesktopappinfo.h>
#endif

#include <locale.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <xlocale.h>

static char *
cf_string_to_utf8 (CFStringRef string)
{
    char buffer[256];

    if (string == NULL ||
        !CFStringGetCString (string, buffer, sizeof (buffer), kCFStringEncodingUTF8))
    {
        return NULL;
    }

    return g_strdup (buffer);
}

/* Applications started from Finder have no locale in their environment;
 * use the language and region chosen in System Settings. */
static void
set_locale_from_macos_preferences (void)
{
    if (g_getenv ("LC_ALL") != NULL || g_getenv ("LC_MESSAGES") != NULL ||
        g_getenv ("LANG") != NULL)
    {
        return;
    }

    CFLocaleRef locale = CFLocaleCopyCurrent ();
    g_autofree char *identifier = cf_string_to_utf8 (CFLocaleGetIdentifier (locale));
    g_autofree char *lang = NULL;
    locale_t available;

    CFRelease (locale);

    if (identifier != NULL)
    {
        /* Drop extensions such as "@rg=…" */
        identifier[strcspn (identifier, "@")] = '\0';
        lang = g_strconcat (identifier, ".UTF-8", NULL);
    }

    /* The region may not match the language, e.g. English in Brazil */
    available = lang != NULL ? newlocale (LC_ALL_MASK, lang, NULL) : NULL;
    if (available != NULL)
    {
        freelocale (available);
        g_setenv ("LANG", lang, TRUE);
    }
    else
    {
        g_setenv ("LANG", "en_US.UTF-8", TRUE);
    }

    /* Translations follow the preferred languages, in order */
    if (g_getenv ("LANGUAGE") == NULL)
    {
        CFArrayRef languages = CFLocaleCopyPreferredLanguages ();
        g_autoptr (GString) list = g_string_new (NULL);

        for (CFIndex i = 0; i < CFArrayGetCount (languages); i++)
        {
            g_autofree char *language = cf_string_to_utf8 (CFArrayGetValueAtIndex (languages, i));

            if (language != NULL)
            {
                g_strdelimit (language, "-", '_');
                g_string_append_printf (list, "%s%s", list->len > 0 ? ":" : "", language);
            }
        }

        CFRelease (languages);

        if (list->len > 0)
        {
            g_setenv ("LANGUAGE", list->str, TRUE);
        }
    }
}
#endif

int
main (int   argc,
      char *argv[])
{
    gint retval;
    NautilusApplication *application;
    /* Initialize gettext support */
#ifdef __APPLE__
    set_locale_from_macos_preferences ();
#endif
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_set_prgname (APPLICATION_ID);

    if (getuid () == 0)
    {
        g_warning (_("Running as root is not supported. "
                     "Consider running `nautilus admin:///` instead."));

        exit (ENOTSUP);
    }

    nautilus_register_resource ();
    /* Run the nautilus application. */
    application = nautilus_application_new ();

    /* hold indefinitely if we're asked to persist */
    if (g_getenv ("NAUTILUS_PERSIST") != NULL)
    {
        g_application_hold (G_APPLICATION (application));
    }

    retval = g_application_run (G_APPLICATION (application),
                                argc, argv);

    g_object_unref (application);

    return retval;
}
