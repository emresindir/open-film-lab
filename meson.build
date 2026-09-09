project('open-film-lab', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: ['c_std=c11', 'warning_level=2'])

subdir('src')

install_data('data/open-film-lab.desktop',
  install_dir: get_option('datadir') / 'applications')

install_data('data/open-film-lab.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')

icon_sizes = ['16', '32', '48', '64', '128', '256', '512', '1024']
foreach s : icon_sizes
  install_data(
    'resources/icons/hicolor' / (s + 'x' + s) / 'apps/open-film-lab.png',
    install_dir: get_option('datadir') / 'icons/hicolor' / (s + 'x' + s) / 'apps'
  )
endforeach