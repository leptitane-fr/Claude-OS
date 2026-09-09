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

cc         = meson.get_compiler('c')
# cos/sin/ceil du cadran de preavis : la libm n'est pas tiree par gtk4.
m_dep      = cc.find_library('m', required : true)
gtk        = dependency('gtk4')
glib       = dependency('glib-2.0')   # le coffre, qui n'a pas d'interface
layershell = dependency('gtk4-layer-shell-0')
giounix    = dependency('gio-unix-2.0')   # GDesktopAppInfo
wlclient   = dependency('wayland-client')
pangocairo = dependency('pangocairo')   # liste des familles installees
# L'ecran de connexion parle a greetd en JSON. Ecrire un analyseur a la main
# pour un protocole dont une erreur d'analyse empeche de se connecter n'est
# pas un pari raisonnable ; json-glib pese 150 Ko et ne depend que de GLib.
jsonglib   = dependency('json-glib-1.0')

# Les mots de passe des lecteurs reseau vont au trousseau, pas dans un
# fichier. libsecret parle au demon gnome-keyring, deja present et deja
# lance sur cette machine : la bibliotheque partagee est installee, seul
# l'en-tete de construction s'ajoute.
secret     = dependency('libsecret-1')

# Le coffre scelle le mot de passe de l'utilisateur avec son code PIN :
# Argon2id pour la derivation, AES-256-GCM pour le scellement. libgcrypt est
# deja sur la machine -- c'est la bibliotheque de GnuPG, tiree par apt
# lui-meme -- et elle porte les deux depuis la 1.11. Aucune dependance
# nouvelle a installer, seul l'en-tete de construction s'ajoute.
#
# ATTENTION : gcry_kdf_derive() ne sait PAS faire Argon2, malgre son nom.
# Voir la tete de src/coffre.c.
gcrypt     = dependency('libgcrypt')

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

# ext-idle-notify-v1 : le compositeur previent quand le siege est inactif.
# C'est le declencheur de la mise en veille progressive (voir energie.h).
#
# Le XML est verse dans le depot alors qu'il EXISTE sur le systeme, dans
# wayland-protocols. C'est volontaire : le meme choix que pour
# foreign-toplevel, pour la meme raison. Le shell se construit alors avec la
# seule dependance wayland-scanner, et une mise a jour de wayland-protocols
# ne peut pas changer sous nos pieds le code genere.
idle_xml = 'protocols/ext-idle-notify-v1.xml'

idle_header = custom_target('idle-header',
  input   : idle_xml,
  output  : 'ext-idle-notify-v1-client-protocol.h',
  command : [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@'])

idle_code = custom_target('idle-code',
  input   : idle_xml,
  output  : 'ext-idle-notify-v1-protocol.c',
  command : [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# ext-session-lock-v1 : le verrou d'ecran. Verse dans le depot comme les
# autres, pour que le code genere ne bouge pas sous nos pieds a la faveur
# d'une mise a jour de wayland-protocols.
verrou_xml = 'protocols/ext-session-lock-v1.xml'

verrou_header = custom_target('verrou-header',
  input   : verrou_xml,
  output  : 'ext-session-lock-v1-client-protocol.h',
  command : [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@'])

verrou_code = custom_target('verrou-code',
  input   : verrou_xml,
  output  : 'ext-session-lock-v1-protocol.c',
  command : [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@'])

pam        = cc.find_library('pam', required : true)
xkb        = dependency('xkbcommon')
cairo      = dependency('cairo')

# Sources communes aux deux composants. La bascule de visibilite doit se
# comporter exactement pareil dans le dock et dans la barre, sinon les deux
# se desynchronisent a l'ecran.
commun = ['src/config.c', 'src/visibility.c', 'src/sysfs.c',
          'src/modes-energie.c']

# Repertoire des feuilles de style, resolu a la compilation.
data_dir = get_option('prefix') / get_option('datadir') / 'claude-os-shell'
add_project_arguments(
  '-DSHELL_DATA_DIR="' + data_dir + '"',
  language : 'c')

executable('claude-os-dock',
  'src/dock.c', 'src/toplevels.c', ftl_header, ftl_code, commun,
  dependencies : [gtk, layershell, giounix, wlclient, pangocairo, m_dep],
  install : true)

# La barre d'etat accueille aussi le module d'energie -- comme elle accueille
# deja le serveur de notifications, et pour la meme raison. Un processus
# separe couterait un runtime GTK4 complet : environ 40 Mo mesures le
# 8 septembre 2026, sur une machine qui n'a que 4 Go soudes et qui swappe.
# La barre est deja residente, deja cliente Wayland, et tient deja la
# connexion logind dont le retroeclairage a besoin.
#
# Contrepartie assumee, la meme que pour les notifications : si la barre
# tombe, la veille tombe avec elle. Elle degrade -- l'ecran reste allume --
# elle ne casse rien.
executable('claude-os-status',
  'src/status.c', 'src/panel.c', 'src/console.c', 'src/notifications.c',
  'src/wifi.c', 'src/bluetooth.c', 'src/energie.c', 'src/retroeclairage.c',
  'src/preavis.c',
  idle_header, idle_code, commun,
  dependencies : [gtk, layershell, giounix, wlclient, pangocairo, m_dep],
  install : true)

executable('claude-os-fond',
  'src/wallpaper.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo],
  install : true)

executable('claude-os-lanceur',
  'src/launcher.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo],
  install : true)

executable('claude-os-connexion',
  'src/connexion.c', 'src/clavier.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo, jsonglib],
  install : true)

# Le correspondant privilegie de l'ecran de connexion : il garde le mot de
# passe scelle par le code PIN, et sert le theme du bureau -- que « _greetd »
# ne peut lire ni l'un ni l'autre, /home etant en 0700.
#
# SANS GTK, et ce n'est pas un detail : il tourne en root, active par socket.
# Un runtime graphique dans un programme privilegie serait une surface
# d'attaque enorme pour un programme qui n'affiche rien.
executable('claude-os-coffre',
  'src/coffre.c',
  dependencies : [glib, jsonglib, gcrypt],
  install : true)

executable('claude-os-fichiers',
  'src/fichiers.c', 'src/fichiers-item.c', 'src/fichiers-ops.c',
  'src/fichiers-lieux.c', 'src/reseau.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo, secret],
  install : true)

executable('claude-os-reglages',
  'src/settings.c', 'src/reseau.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo, secret],
  install : true)

# Connexion des lecteurs reseau a l'ouverture de session. Sans GTK : voir la
# tete du fichier.
executable('claude-os-lecteurs-auto',
  'src/lecteurs-auto.c', 'src/reseau.c',
  dependencies : [giounix, secret],
  install : true)

# LE VERROU N'EST PAS UNE APPLICATION GTK, et c'est explique en tete de
# verrou.c : le protocole demande une surface d'un type que GTK ne sait pas
# produire, et aucune bibliotheque equivalente a gtk4-layer-shell n'existe
# pour le verrouillage dans Debian trixie. D'ou wayland-client, cairo et
# pango a nu -- mais pas gtk.
executable('claude-os-verrou',
  'src/verrou.c', verrou_header, verrou_code,
  dependencies : [wlclient, xkb, cairo, pangocairo, giounix, jsonglib, pam],
  install : true)

install_data(['data/claude-os-reglages.desktop',
              'data/claude-os-fichiers.desktop'],
  install_dir : get_option('prefix') / get_option('datadir') / 'applications')

# L'icône de la cloche, livrée avec le shell.
#
# DANS NOTRE PROPRE RÉPERTOIRE, ET PAS DANS /usr/share/icons/hicolor.
#
# Posée dans hicolor, elle dépendait de « gtk4-update-icon-cache » : le cache
# du thème existait déjà, GTK le lisait de préférence au répertoire, et
# l'icône restait introuvable — mesuré le 8 septembre 2026, la cloche
# s'affichait en carré « image manquante » même après régénération du cache.
#
# Le shell ajoute donc ce chemin lui-même au démarrage (config.c). Le même
# mécanisme que pour la feuille de style : l'asset voyage avec le binaire, et
# aucune étape d'installation ne peut l'oublier.
install_data(
  'data/icons/hicolor/index.theme',
  install_dir : data_dir / 'icons' / 'hicolor')
install_data(
  'data/icons/hicolor/scalable/status/claude-os-cloche-symbolic.svg',
  install_dir : data_dir / 'icons' / 'hicolor' / 'scalable' / 'status')

install_subdir('style',
  install_dir : data_dir,
  strip_directory : false)
