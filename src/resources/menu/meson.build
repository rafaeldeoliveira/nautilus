# Only used on macOS, see ../meson.build
macos_ui_files += custom_target('nautilus-pathbar-context-menu.ui',
  input: 'nautilus-pathbar-context-menu.ui',
  output: 'nautilus-pathbar-context-menu.ui',
  command: [macos_shortcuts, 'rewrite', '@INPUT@', '@OUTPUT@'],
)
