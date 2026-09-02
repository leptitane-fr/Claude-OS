project('claude-os-shell', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    # Optimise pour la taille : le shell doit rester leger en memoire, et la
    # vitesse brute n'est jamais le facteur limitant d'une interface.
    'optimization=s',
    'b_lto=true',
  ])

gtk        = dependency('gtk4')
layershell = dependency('gtk4-layer-shell-0')
giounix    = dependency('gio-unix-2.0')   # GDesktopAppInfo
wlclient   = dependency('wayland-client')
pangocairo = dependency('pangocairo')   # liste des familles installees

# wlr-foreign-toplevel-management-v1 : le protocole qui dit quelles fenetres
# sont ouvertes. Il n'est empaquete nulle part dans Debian trixie -- aucun
# paquet wlr-protocols -- le XML est donc verse dans le depot et le code
# client genere ici. Une dependance de moins a la construction de l'image.
wlscanner = find_program('wayland-scanner')
ftl_xml   = 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml'

ftl_header = custom_target('ftl-header',
  input   : ftl_xml,
  output  : 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  command : [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@'])

ftl_code = custom_target('ftl-code',
  input   : ftl_xml,
  output  : 'wlr-foreign-toplevel-management-unstable-v1-protocol.c',
  command : [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# Sources communes aux deux composants. La bascule de visibilite doit se
# comporter exactement pareil dans le dock et dans la barre, sinon les deux
# se desynchronisent a l'ecran.
commun = ['src/config.c', 'src/visibility.c', 'src/sysfs.c']

# Repertoire des feuilles de style, resolu a la compilation.
data_dir = get_option('prefix') / get_option('datadir') / 'claude-os-shell'
add_project_arguments(
  '-DSHELL_DATA_DIR="' + data_dir + '"',
  language : 'c')

executable('claude-os-dock',
  'src/dock.c', 'src/toplevels.c', ftl_header, ftl_code, commun,
  dependencies : [gtk, layershell, giounix, wlclient, pangocairo],
  install : true)

executable('claude-os-status',
  'src/status.c', 'src/panel.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo],
  install : true)

executable('claude-os-fond',
  'src/wallpaper.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo],
  install : true)

executable('claude-os-reglages',
  'src/settings.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo],
  install : true)

install_data('data/claude-os-reglages.desktop',
  install_dir : get_option('prefix') / get_option('datadir') / 'applications')

install_subdir('style',
  install_dir : data_dir,
  strip_directory : false)
