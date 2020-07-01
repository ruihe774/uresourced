configure_file(
    input: 'uresourced.service.in',
    output: 'uresourced.service',
    configuration: unit_conf,
    install: true,
    install_dir: systemd_userunitdir
)

# Pre-define a wants for the user-service
meson.add_install_script(
    '../meson-add-wants.sh',
    systemd_userunitdir,
    'graphical-session.target.wants/',
    'uresourced.service'
)

user_drop_ins = [
  ['session.slice', 'session.slice'],

  # Move important services into session slice
  ['at-spi-dbus-bus.service', 'move-into-session-slice.service'],
  ['dbus-broker.service', 'move-into-session-slice.service'],
  ['gnome-shell-.service', 'move-into-session-slice.service'],
  ['gsd-.service', 'move-into-session-slice.service'],
  ['gvfs-.service', 'move-into-session-slice.service'],
  ['pipewire.service', 'move-into-session-slice.service'],
  ['pulseaudio.service', 'move-into-session-slice.service'],

  # Move things into app.slice
  ['gnome-launched-.scope', 'move-into-app-slice.scope'],

  # flatpak is already fixed upstream (as of 2020-06)
  ['flatpak-.scope', 'move-into-app-slice.scope'],

  # The following likely still need upstream changes (as of 2020-06)
  ['dbus-.service', 'move-into-app-slice.service'],
  ['evolution-.service', 'move-into-app-slice.service'],
  ['tracker-.service', 'move-into-app-slice.service'],
  ['zeitgeist.service', 'move-into-app-slice.service'],
  ['zeitgeist-.service', 'move-into-app-slice.service'],
]

foreach drop_in: user_drop_ins
    for_unit = drop_in[0]
    origin = drop_in[1]

    install_data(
        '@0@'.format(origin),
        rename: '00-uresourced.conf',
        install_dir: join_paths(systemd_userunitdir, '@0@.d'.format(for_unit))
    )
endforeach

