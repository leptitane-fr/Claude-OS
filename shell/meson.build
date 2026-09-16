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

# Sources communes a tous les composants.
#
# visibility.c n'y est plus : la decision « a l'ecran ou non » ne vit que
# dans le dock, qui entraine la barre par le bus. Deux automates, un par
# processus, finissaient par se contredire. glissiere.c, l'animation de
# sortie, n'est lie qu'aux deux composants qui s'en servent.
commun = ['src/config.c', 'src/sysfs.c', 'src/modes-energie.c',
          'src/abris-batterie.c', 'src/actions-capot.c']

# Repertoire des feuilles de style, resolu a la compilation.
data_dir = get_option('prefix') / get_option('datadir') / 'claude-os-shell'
add_project_arguments(
  '-DSHELL_DATA_DIR="' + data_dir + '"',
  language : 'c')

# Le clavier à l'écran de la session (clavier-ecran.c, saisie.c) : deux
# protocoles de wlroots 0.18.2, la version de labwc, versés dans le dépôt
# comme les autres — aucun paquet Debian ne les porte. clavier.c est celui
# de l'écran de connexion, dont le dock reprend le pavé numérique.
im_xml = 'protocols/input-method-unstable-v2.xml'
vk_xml = 'protocols/virtual-keyboard-unstable-v1.xml'

im_header = custom_target('im-header',
  input   : im_xml,
  output  : 'input-method-unstable-v2-client-protocol.h',
  command : [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@'])

im_code = custom_target('im-code',
  input   : im_xml,
  output  : 'input-method-unstable-v2-protocol.c',
  command : [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@'])

vk_header = custom_target('vk-header',
  input   : vk_xml,
  output  : 'virtual-keyboard-unstable-v1-client-protocol.h',
  command : [wlscanner, 'client-header', '@INPUT@', '@OUTPUT@'])

vk_code = custom_target('vk-code',
  input   : vk_xml,
  output  : 'virtual-keyboard-unstable-v1-protocol.c',
  command : [wlscanner, 'private-code', '@INPUT@', '@OUTPUT@'])

# retourneur.c et etabli.c sont les deux faces de la surface d'outils
# (docs/14) : celui qui fait tourner le dock, et celui qui dessine ce qu'une
# application a declare.
executable('claude-os-dock',
  'src/dock.c', 'src/toplevels.c', 'src/visibility.c', 'src/glissiere.c',
  'src/retourneur.c', 'src/etabli.c', 'src/auvent.c',
  'src/tablette.c', 'src/rotation.c',
  'src/clavier-ecran.c', 'src/clavier.c', 'src/saisie.c', 'src/mots.c',
  ftl_header, ftl_code, im_header, im_code, vk_header, vk_code, commun,
  dependencies : [gtk, layershell, giounix, wlclient, pangocairo, xkb, m_dep],
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
  'src/avis.c', 'src/batterie.c', 'src/capot.c',
  'src/logind.c', 'src/coin.c', 'src/tiroir.c', 'src/toplevels.c',
  idle_header, idle_code, ftl_header, ftl_code, commun,
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

# gdk-pixbuf est déjà tiré par GTK 4 ; il est nommé ici parce qu'on s'en sert
# directement — pour décoder réduit, lire l'orientation EXIF, animer les GIF
# et écrire les vignettes de Fichiers — et qu'une dépendance qu'on utilise
# sans la déclarer casse le jour où GTK cesse de l'apporter.
gdkpixbuf = dependency('gdk-pixbuf-2.0')

# outils.c : Fichiers est le premier client de la surface d'outils (docs/14).
# Toute application qui publie une barre le lie de la meme facon.
executable('claude-os-fichiers',
  'src/fichiers.c', 'src/fichiers-item.c', 'src/fichiers-ops.c',
  'src/fichiers-lieux.c', 'src/fichiers-apercu.c', 'src/outils.c',
  'src/reseau.c', 'src/nuage.c', commun,
  dependencies : [gtk, layershell, giounix, pangocairo, secret, gdkpixbuf],
  install : true)

# La visionneuse d'images.

executable('claude-os-images',
  'src/images.c', 'src/images-vue.c', commun,
  dependencies : [gtk, giounix, pangocairo, gdkpixbuf, m_dep],
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

# Montage des lecteurs nuage a l'ouverture de session. Sans GTK, pour la
# raison donnee en tete du fichier.
executable('claude-os-nuage-auto',
  'src/nuage-auto.c', 'src/nuage.c',
  dependencies : [giounix],
  install : true)

# La phrase qui ouvre la configuration chiffree de rclone. Ni GTK ni GIO :
# libsecret et la libc suffisent. Voir la tete du fichier pour la raison
# d'etre de ce detour -- un jeton de rafraichissement vaut un mot de passe,
# et ne doit donc pas dormir en clair dans ~/.config.
executable('claude-os-nuage-phrase',
  'src/nuage-phrase.c',
  dependencies : [glib, secret],
  install : true)

# LE CONTRAT DES OUTILS, des deux cotes.
#
# outils.c est la moitie « application » : elle est liee a chaque
# application qui publie une barre. claude-os-outils est l'autre moitie,
# celle du diagnostic -- il lit une barre publiee, sait jouer le dock, et
# porte l'application temoin qui sert d'exemple de reference.
#
# SANS GTK, et c'est voulu : un outil de diagnostic qui tire un runtime
# graphique complet ne peut pas servir le jour ou c'est le graphique qui est
# en panne. Le contrat lui-meme ne demande rien de graphique -- le temoin en
# est la preuve, il tourne sur un GApplication tout court.
executable('claude-os-outils',
  'src/outils-diag.c', 'src/outils.c',
  dependencies : [giounix],
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

# LE LECTEUR VIDEO.
#
# FFmpeg plutot que libmpv ou GStreamer, et ce choix a ete MESURE avant
# d'etre fait -- voir docs/11. Les codecs sont dans libavcodec : une fois le
# lecteur installe, il n'y a plus jamais de « codec manquant ».
#
# libva et libdrm ne sont pas des dependances decoratives : c'est par elles
# que l'image decodee arrive a l'ecran SANS JAMAIS ETRE RECOPIEE
# (av_hwframe_map vers un dmabuf, puis GdkDmabufTexture dans un
# GtkGraphicsOffload). Mesure sur MADOO, mire 1080p30, repos a 2,92 W :
# ce chemin coute 3,24 W, le chemin naif -- meme decodage materiel, mais
# suivi de av_hwframe_transfer_data -- en coute 5,45.
avcodec    = dependency('libavcodec')
avformat   = dependency('libavformat')
avutil     = dependency('libavutil')
swscale    = dependency('libswscale')     # repli logiciel seulement
swresample = dependency('libswresample')  # conversion audio vers PipeWire
libdrm     = dependency('libdrm')         # drm_fourcc.h : NV12, P010
pipewire   = dependency('libpipewire-0.3')

executable('claude-os-video',
  'src/video.c', 'src/video-moteur.c', 'src/video-image.c',
  'src/video-audio.c', 'src/video-bibliotheque.c', commun,
  dependencies : [gtk, giounix, pangocairo,
                  avcodec, avformat, avutil, swscale, swresample,
                  libdrm, pipewire, m_dep],
  install : true)

install_data(['data/claude-os-reglages.desktop',
              'data/os.claude.shell.fichiers.desktop',
              'data/os.claude.shell.images.desktop',
              'data/os.claude.shell.video.desktop'],
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
# La cloche, et les trois glyphes des modes d'energie. TOUS engendres sauf
# la cloche, par tools/fabrique-icones.py -- voir la section « LES ICONES
# LIVREES AVEC LE SHELL » de ce fichier pour la raison qui les met la plutot
# que dans le theme d'icones.
install_data(
  ['data/icons/hicolor/scalable/status/claude-os-cloche-symbolic.svg',
   'data/icons/hicolor/scalable/status/claude-os-mode-travail-symbolic.svg',
   'data/icons/hicolor/scalable/status/claude-os-mode-automatique-symbolic.svg',
   'data/icons/hicolor/scalable/status/claude-os-mode-nomade-symbolic.svg'],
  install_dir : data_dir / 'icons' / 'hicolor' / 'scalable' / 'status')

# Le dictionnaire des suggestions du clavier à l'écran : 119 688 formes du
# français avec leur fréquence, tirées de Lexique 3.83 (CC BY-SA) par
# tools/fabrique-mots.py. 1,5 Mo, projeté en lecture seule par mots.c — il
# reste dans le cache de pages, jamais recopié.
# suites-fr.txt : quel mot suit quel autre (Tatoeba, CC BY 2.0 FR). C'est
# lui qui fait la différence entre un dictionnaire et un clavier.
install_data(['data/mots-fr.txt', 'data/suites-fr.txt'], install_dir : data_dir)

install_subdir('style',
  install_dir : data_dir,
  strip_directory : false)

# POSER UN .desktop NE SUFFIT PAS : le systeme lit « mimeinfo.cache », que
# seul « update-desktop-database » regenere. Sans cette etape, une
# application installee avec ses types declares reste introuvable, et
# Fichiers repond « aucune application n'est installee pour ce type de
# fichier ». Constate sur MADOO le 10 septembre 2026 -- le lecteur video
# etait en place et son cache datait de deux jours plus tot.
meson.add_install_script('data/apres-installation.sh')
