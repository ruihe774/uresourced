project('uresourced', 'c',
          version: '0.3.0',
    meson_version: '>= 0.50.0',
  default_options: [ 'warning_level=2',
                     'c_std=gnu11',
                   ],
)

prefix = get_option('prefix')
sysconfdir = join_paths(prefix, get_option('sysconfdir'))
datadir = join_paths(prefix, get_option('datadir'))

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('SYSCONFDIR', sysconfdir)
configure_file(
  output: 'uresourced-config.h',
  configuration: config_h,
)
add_project_arguments([
  '-I' + meson.build_root(),
], language: 'c')

libexecdir = join_paths(get_option('prefix'), get_option('libexecdir'))

gnome  = import('gnome')

systemd_dep = dependency('systemd')

if get_option('systemdsystemunitdir') != ''
  systemd_systemunitdir = get_option('systemdsystemunitdir')
else
  systemd_systemunitdir = systemd_dep.get_pkgconfig_variable('systemdsystemunitdir')
endif

if get_option('systemduserunitdir') != ''
  systemd_userunitdir = get_option('systemduserunitdir')
else
  systemd_userunitdir = systemd_dep.get_pkgconfig_variable('systemduserunitdir',
                                                           define_variable: ['prefix', get_option('prefix')])
endif

# Detect whether we have memory_recursiveprot
if systemd_dep.version().version_compare('>=247')
  systemd_uses_recursiveprot=true
else
  systemd_uses_recursiveprot=false
endif

subdir('data')
subdir('src')
