# On macOS, menus and the shortcuts dialog show Command-based shortcuts, to
# match nautilus_shortcut_for_platform(). Rewritten copies of the .ui files
# in the build directory take precedence over the sources.
macos_ui_files = []
if host_machine.system() == 'darwin'
  macos_shortcuts = find_program(meson.project_source_root() / 'build-aux' / 'macos' / 'macos-shortcuts.py')
  subdir('menu')
endif

subdir('ui')

gresource_file = configure_file(
  input: 'nautilus.gresource.xml.in',
  output: 'nautilus.gresource.xml',
  configuration: file_conf
)

resources = gnome.compile_resources(
  'nautilus-resources',
  gresource_file,
  dependencies: [appdata, blueprints, gresource_file, macos_ui_files],
  source_dir: 'resources',
  c_name: 'nautilus',
  extra_args: '--manual-register'
)

resources_header = []

foreach res : resources
  filename = res.full_path().split('/')[-1]
  if filename == 'nautilus-resources.h'
    resources_header += res
  endif
endforeach
