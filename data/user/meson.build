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

user_drop_ins = []

if not systemd_uses_recursiveprot
  user_drop_ins += [
    ['session.slice', 'session.slice'],
  ]
endif

user_drop_ins += [
  # Move important services into session slice
  ['at-spi-dbus-bus.service', 'move-into-session-slice.service'],
  ['dbus-broker.service', 'move-into-session-slice.service'],
  ['gnome-shell-.service', 'move-into-session-slice.service'],
  ['gsd-.service', 'move-into-session-slice.service'],
  ['gvfs-.service', 'move-into-session-slice.service'],
  ['pipewire.service', 'move-into-session-slice.service'],
  ['pulseaudio.service', 'move-into-session-slice.service'],
  # Move selected core applications into session.slice (useful for recovery)
  ['gnome-launched-gnome-system-monitor.desktop-.scope', 'move-into-session-slice.scope'],
  ['gnome-launched-org.gnome.Usage.desktop-.scope', 'move-into-session-slice.scope'],

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

if get_option('cgroupify')
# This renames any file/directory to change % to \.
# This is a horrible solution, but meson insists on translating \ to / in
# command lines so we cannot install the file into the right location.
    meson.add_install_script(
        '../meson-unescape-dropin-dirs.sh',
        systemd_userunitdir
    )

    configure_file(
        input: 'cgroupify@.service.in',
        output: 'cgroupify@.service',
        configuration: unit_conf,
        install: true,
        install_dir: systemd_userunitdir
    )

    cgroupify_appid_scope_pre = [
        # app-gnome- is the old gnome-desktop code (which should be killed)
        'app-gnome-',
        # app-glib- is glib (patch is not merged upstream)
        'app-glib-',
        # KDE with kio
        'app-',
    ]
    cgroupify_appid_service_pre = [
        # KDE with kio
        'app-',
    ]

    # Special characters such as '-' must be escaped.
    # We use % instead of \ here, and the directories are moved by
    # meson-unescape-dropin-dirs.sh!
    cgroupify_appids = [
        'chromium%x2dbrowser',
        'firefox',
        'org.gnome.Epiphany',
        'google%x2dchrome',
    ]

    cgroupify_drop_ins = []

    foreach appid: cgroupify_appids
        foreach prefix: cgroupify_appid_scope_pre
            cgroupify_drop_ins += [
                ['@0@@1@-.scope'.format(prefix, appid), 'cgroupify.scope']
            ]
        endforeach

        foreach prefix: cgroupify_appid_service_pre
            cgroupify_drop_ins += [
                ['@0@@1@@@.service'.format(prefix, appid), 'cgroupify.service']
            ]
        endforeach
    endforeach

    foreach drop_in: cgroupify_drop_ins
        for_unit = drop_in[0]
        origin = drop_in[1]

        install_data(
            '@0@'.format(origin),
            rename: '99-cgroupify.conf',
            install_dir: join_paths(systemd_userunitdir, '@0@.d'.format(for_unit))
        )
    endforeach
endif
