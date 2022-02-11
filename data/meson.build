
unit_conf = configuration_data()
unit_conf.set('libexecdir', libexecdir)

subdir('system')
subdir('user')

install_data(['org.freedesktop.UResourced.conf'],
  install_dir : join_paths(datadir, 'dbus-1', 'system.d')
)

conf_conf = configuration_data()
if get_option('appmanagement')
    conf_conf.set('appboost_comment', '')
else
    conf_conf.set('appboost_comment', '\n# NOTE: App management support was disabled.\n# This configuration section is likely unused\n#')
endif

configure_file(
    input: 'uresourced.conf.in',
    output: 'uresourced.conf',
    configuration: conf_conf,
    install: true,
    install_dir: sysconfdir,
)
