#!/usr/bin/env python3
"""Rewrite the keyboard shortcuts shown in GtkBuilder .ui files for macOS.

Usage:
    macos-shortcuts.py rewrite INPUT OUTPUT
    macos-shortcuts.py wrap OUTDIR -- COMMAND...   (run COMMAND, then rewrite
                                                  the .ui files in OUTDIR)

Used by src/resources/meson.build on macOS for menu "accel" attributes and
the shortcuts dialog. The shortcuts themselves are translated at runtime by
nautilus_shortcut_for_platform() (src/nautilus-shortcuts.c); the rules here
must describe the same shortcuts.
"""

import os
import re
import subprocess
import sys

MODIFIERS = {
    'control': 'Control', 'ctrl': 'Control', 'primary': 'Control',
    'alt': 'Alt', 'mod1': 'Alt', 'shift': 'Shift', 'meta': 'Meta',
    'super': 'Super',
}
ORDER = ['Control', 'Shift', 'Alt', 'Meta', 'Super']

# (modifiers, key) -> macOS (modifiers, key), or None when there is no
# equivalent. Mirrors mac_shortcuts[] in src/nautilus-shortcuts.c.
EXCEPTIONS = {
    (('Control',), 'h'): (('Meta', 'Shift'), 'period'),   # Command-H hides the app
    (('Alt',), 'Left'): (('Meta',), 'bracketleft'),       # Option-arrows move by word
    (('Alt',), 'Right'): (('Meta',), 'bracketright'),
    (('Alt',), 'Up'): (('Meta',), 'Up'),
    (('Alt',), 'Down'): (('Meta',), 'Down'),
    (('Alt',), 'Home'): (('Meta', 'Shift'), 'h'),
}
DIGITS = set('0123456789')
# Tab navigation stays on Control, as in libadwaita and Safari
KEEP_CONTROL = {'Page_Up', 'Page_Down', 'KP_Page_Up', 'KP_Page_Down', 'Tab', 'ISO_Left_Tab'}

# Shortcuts dialog entries for keys Mac keyboards mostly lack. Delete still
# works (fn-Backspace), but Finder's keys come first.
DIALOG_EXTRAS = {
    'Delete': '<Meta>BackSpace Delete',
    '<Shift>Delete': '<Meta><Alt>BackSpace <Shift>Delete',
}


def parse(accel):
    mods = []
    rest = accel
    while (m := re.match(r'<([A-Za-z0-9]+)>', rest)):
        mods.append(MODIFIERS[m.group(1).lower()])
        rest = rest[m.end():]
    return tuple(sorted(set(mods), key=ORDER.index)), rest


def unparse(mods, key):
    return ''.join(f'<{m}>' for m in sorted(mods, key=ORDER.index)) + key


def translate(accel):
    """One accelerator for macOS, or None if it has no equivalent."""
    if not accel.startswith('<'):
        return accel
    mods, key = parse(accel)
    first = key.split('...')[0]          # ranges such as "0...8"
    if mods == ('Alt',) and first in DIGITS:
        return None                      # Option-digits type characters
    for (emods, ekey), (nmods, nkey) in EXCEPTIONS.items():
        # Letter keys are case-insensitive in accelerators ("<Primary>H")
        same_key = ekey.lower() == key.lower() if len(key) == 1 else ekey == key
        if set(emods) == set(mods) and same_key:
            return unparse(nmods, nkey)
    if 'Control' in mods and first not in KEEP_CONTROL:
        mods = tuple('Meta' if m == 'Control' else m for m in mods)
    return unparse(mods, key)


def translate_list(value, sep):
    out = [t for t in (translate(a) for a in value.split(sep) if a) if t is not None]
    return sep.join(out) if out else None


def unescape(s):
    return s.replace('&lt;', '<').replace('&gt;', '>')


def escape(s):
    return s.replace('<', '&lt;').replace('>', '&gt;')


def rewrite_ui(ui):
    # Shortcuts dialog items: space-separated alternatives; drop items
    # that have no macOS equivalent at all.
    def item(m):
        block = m.group(0)
        acc = re.search(r'(<property name="accelerator"[^>]*>)([^<]*)(</property>)', block)
        if acc is None:
            return block
        value = unescape(acc.group(2))
        normalized = unparse(*parse(value)) if value.startswith('<') else value
        if normalized in DIALOG_EXTRAS:
            new = DIALOG_EXTRAS[normalized]
        else:
            new = translate_list(value, ' ')
        if new is None:
            return ''
        return block.replace(acc.group(0), acc.group(1) + escape(new) + acc.group(3))

    ui = re.sub(r'<child>\s*<object class="AdwShortcutsItem">.*?</object>\s*</child>', item, ui, flags=re.S)

    # Menu items and shortcut triggers
    def attr(m):
        sep = '|' if 'trigger' in m.group(1) else ' '
        new = translate_list(unescape(m.group(2)), sep)
        return '' if new is None else m.group(1) + escape(new) + m.group(3)

    return re.sub(r'(<(?:attribute name="accel"|property name="trigger")[^>]*>)([^<]*)(</(?:attribute|property)>)',
                  attr, ui)


def rewrite_file(src, dest):
    with open(src, encoding='utf-8') as f:
        ui = f.read()
    new = rewrite_ui(ui)
    if src != dest or new != ui:
        with open(dest, 'w', encoding='utf-8') as f:
            f.write(new)


def main():
    if sys.argv[1] == 'rewrite':
        rewrite_file(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'wrap':
        outdir = sys.argv[2]
        command = sys.argv[sys.argv.index('--') + 1:]
        subprocess.run(command, check=True)
        for name in os.listdir(outdir):
            if name.endswith('.ui'):
                path = os.path.join(outdir, name)
                rewrite_file(path, path)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
