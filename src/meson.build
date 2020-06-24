uresourced_sources = [
  'main.c',
  'r-manager.c',
  'r-sd-login.c',
  'utils.c',
]

uresourced_deps = [
  dependency('glib-2.0'),
  dependency('gobject-2.0'),
  dependency('gio-2.0'),
  dependency('libsystemd'),
]

executable('uresourced', uresourced_sources,
  dependencies: uresourced_deps,
  install: true,
  install_dir: libexecdir,
)
