cgroupify_sources = [
  'cgroupify.c',
]

cgroupify_deps = [
  dependency('libsystemd'),
]

executable('cgroupify',
  cgroupify_sources,
  dependencies: cgroupify_deps,
  install: true,
  install_dir: libexecdir,
)
