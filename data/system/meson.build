configure_file(
    input: 'uresourced.service.in',
    output: 'uresourced.service',
    configuration: unit_conf,
    install: true,
    install_dir: systemd_systemunitdir
)

system_drop_ins = [
  ['user@.service', 'user@.service'],
]

foreach drop_in: system_drop_ins
    for_unit = drop_in[0]
    origin = drop_in[1]

    install_data(
        '@0@'.format(origin),
        rename: '00-uresourced.conf',
        install_dir: join_paths(systemd_systemunitdir, '@0@.d'.format(for_unit))
    )
endforeach
