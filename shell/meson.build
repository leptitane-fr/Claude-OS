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
  'src/dock.c', commun,
  dependencies : [gtk, layershell, giounix],
  install : true)

executable('claude-os-status',
  'src/status.c', 'src/panel.c', commun,
  dependencies : [gtk, layershell, giounix],
  install : true)

install_subdir('style',
  install_dir : data_dir,
  strip_directory : false)
