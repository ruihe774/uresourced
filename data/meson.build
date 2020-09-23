
unit_conf = configuration_data()
unit_conf.set('libexecdir', libexecdir)

subdir('system')
subdir('user')

install_data(['org.freedesktop.UResourced.conf'],
  install_dir : join_paths(datadir, 'dbus-1', 'system.d')
)

install_data(
    'uresourced.conf',
    install_dir: sysconfdir
)
