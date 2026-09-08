#!/usr/bin/env bash
# Claude OS -- acces root natif de Claude Desktop.
#
# A lancer depuis la machine, dans un clone a jour du depot :
#   sudo bash install/patch-acces-root-claude.sh
#   sudo bash install/patch-acces-root-claude.sh --dry-run   # montre tout, ne fait rien
#   sudo bash install/patch-acces-root-claude.sh --retirer   # defait le correctif
#
# Options : --user <compte>   designer le compte du bureau explicitement
#           --sans-btrfs      ne pas creer le depot d'instantanes
#
# Ce que le correctif met en place :
#   - le guichet /usr/local/bin/claude-os-root, seul chemin eleve sans mot de passe ;
#   - la fenetre de confirmation des actions sensibles (claude-os-askpass) ;
#   - le depot d'instantanes btrfs, qui rend les ecritures annulables ;
#   - le journal des actions privilegiees ;
#   - les instructions correspondantes dans le ~/.claude/CLAUDE.md du compte.
#
# Le script ne redemarre pas, et ne ferme pas la session : la connexion
# courante reste disponible pour lire son resultat et, au besoin, restaurer
# la sauvegarde indiquee a la fin.
set -Eeuo pipefail

DRY=0
RETIRER=0
USER_OPT=""
SNAPSHOTS=1

usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; }

while (($#)); do
    case "$1" in
        --dry-run)       DRY=1 ;;
        --retirer)       RETIRER=1 ;;
        --user)          USER_OPT="${2:-}"; shift ;;
        --sans-btrfs)    SNAPSHOTS=0 ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "Option inconnue : $1" >&2; exit 2 ;;
    esac
    shift
done

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
GROUPE=claude-os-root
REGLE=/etc/sudoers.d/99-claude-os-root
GUICHET=/usr/local/bin/claude-os-root
SENSIBLE=/usr/local/bin/claude-os-sensible
JOURNAL_DIR=/var/log/claude-os
REF=/usr/share/claude-os/politique

die()  { printf '\n\033[31mERREUR : %s\033[0m\n' "$*" >&2; exit 1; }
note() { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }
warn() { printf '   \033[33m! %s\033[0m\n' "$*"; }
good() { printf '   \033[32m+ %s\033[0m\n' "$*"; }

run() {
    if ((DRY)); then
        printf '   \033[2m$'; printf ' %q' "$@"; printf '\033[0m\n'
    else
        "$@"
    fi
}

# Ecriture de fichier en mode simulation : on montre le chemin et le contenu
# plutot qu'une redirection illisible.
ecrire() {
    local cible="$1" mode="${2:-0644}"
    if ((DRY)); then
        printf '   \033[2m$ cat > %s   (mode %s)\033[0m\n' "$cible" "$mode"
        sed 's/^/   \x1b[2m|\x1b[0m /' 
    else
        mkdir -p "$(dirname "$cible")"
        cat > "$cible"
        chmod "$mode" "$cible"
    fi
}

((EUID == 0)) || die "lancer avec sudo :  sudo bash install/patch-acces-root-claude.sh"

# ------------------------------------------------------------- compte cible
TARGET_USER="${USER_OPT:-}"
if [[ -z "$TARGET_USER" && -r /etc/claude-os/utilisateur ]]; then
    TARGET_USER="$(head -n1 /etc/claude-os/utilisateur)"
fi
[[ -z "$TARGET_USER" ]] && TARGET_USER="${SUDO_USER:-}"
[[ -n "$TARGET_USER" && "$TARGET_USER" != "root" ]] || die "compte cible non identifie.
      Depuis le compte du bureau :  sudo bash install/patch-acces-root-claude.sh
      Depuis root                :  bash install/patch-acces-root-claude.sh --user <compte>"
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)" || TARGET_HOME=""
[[ -n "$TARGET_HOME" && -d "$TARGET_HOME" ]] || die "compte « $TARGET_USER » inconnu, ou sans repertoire personnel."

STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="/root/claude-os-root-backup-$STAMP"

echo
echo "Claude OS — acces root natif de Claude Desktop"
info "compte du bureau : $TARGET_USER ($TARGET_HOME)"
info "depot            : $REPO_DIR"
((DRY)) && warn "SIMULATION : rien ne sera modifie."

#==============================================================================
# RETRAIT
#==============================================================================
if ((RETIRER)); then
    note "Retrait du correctif"
    # L'ordre compte : la regle sudo d'abord. Tant qu'elle est la, le guichet
    # reste utilisable sans mot de passe, meme a moitie desinstalle.
    if [[ -f "$REGLE" ]]; then
        run rm -f "$REGLE"; good "regle sudo retiree"
    else
        info "regle sudo deja absente"
    fi
    if getent group "$GROUPE" >/dev/null 2>&1; then
        if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$GROUPE"; then
            run gpasswd -d "$TARGET_USER" "$GROUPE" >/dev/null
            good "$TARGET_USER retire du groupe $GROUPE"
        fi
        run groupdel "$GROUPE" 2>/dev/null || true
    fi
    if [[ -f /etc/sudo.conf ]] && grep -q 'claude-os-askpass' /etc/sudo.conf; then
        if ((DRY)); then
            info "ligne askpass retiree de /etc/sudo.conf"
        else
            sed -i '/claude-os-askpass/d; /Claude OS : confirmation humaine/d' /etc/sudo.conf
        fi
        good "ligne askpass retiree de /etc/sudo.conf"
    fi
    for f in "$SENSIBLE" "$GUICHET" /usr/local/bin/claude-os /usr/local/bin/claude-os-askpass; do
        [[ -e "$f" ]] && { run rm -f "$f"; good "$(basename "$f") retire"; }
    done
    CLAUDE_MD="$TARGET_HOME/.claude/CLAUDE.md"
    if [[ -f "$CLAUDE_MD" ]] && grep -q 'claude-os:debut' "$CLAUDE_MD"; then
        if ((DRY)); then
            info "bloc retire de $CLAUDE_MD"
        else
            TMP="$(mktemp)"
            awk '/<!-- claude-os:debut/{s=1} !s{print} /<!-- claude-os:fin/{s=0}' \
                "$CLAUDE_MD" > "$TMP"
            cat "$TMP" > "$CLAUDE_MD"; rm -f "$TMP"
        fi
        good "bloc retire de ~/.claude/CLAUDE.md"
    fi
    echo
    info "Conserves volontairement : les instantanes, le journal"
    info "($JOURNAL_DIR) et /etc/claude-os/politique.conf."
    info "Un retrait ne doit pas effacer les traces de ce qui a ete fait."
    echo
    exit 0
fi

#==============================================================================
# PREALABLES
#==============================================================================
note "Prealables"

if [[ -r /etc/os-release ]]; then
    # shellcheck source=/dev/null
    . /etc/os-release
    info "systeme : ${PRETTY_NAME:-inconnu}"
fi

MANQUE=()
command -v sudo    >/dev/null 2>&1 || MANQUE+=(sudo)
command -v btrfs   >/dev/null 2>&1 || MANQUE+=(btrfs-progs)
command -v logger  >/dev/null 2>&1 || MANQUE+=(bsdutils)
if ((${#MANQUE[@]})); then
    info "paquets a installer : ${MANQUE[*]}"
    run apt-get update -qq
    run env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${MANQUE[@]}" \
        || die "installation impossible. Sans sudo, aucun acces root delegue n'est possible."
    good "${MANQUE[*]} installes"
else
    info "sudo, btrfs-progs et logger sont la"
fi

command -v foot >/dev/null 2>&1 \
    || warn "foot absent : la fenetre de confirmation du niveau 3 ne pourra pas s'ouvrir."

# Le groupe est cree ici, et non avec la regle sudo : le journal, plus bas,
# lui appartient.
run groupadd -f "$GROUPE"

for f in claude-os-root claude-os claude-os-askpass; do
    [[ -f "$REPO_DIR/rootfs/usr/local/bin/$f" ]] \
        || die "$f introuvable dans le depot. Le clone est-il a jour ? (git pull)"
done

#==============================================================================
# SAUVEGARDE
#==============================================================================
note "Sauvegarde recuperable"
run mkdir -p "$BACKUP"
sauver() {
    local chemin="$1"
    [[ -e "$chemin" || -L "$chemin" ]] || return 0
    run mkdir -p "$BACKUP/$(dirname "${chemin#/}")"
    run cp -a -- "$chemin" "$BACKUP/${chemin#/}"
}
for p in "$REGLE" "$GUICHET" "$SENSIBLE" /usr/local/bin/claude-os /usr/local/bin/claude-os-askpass \
         /etc/claude-os/politique.conf /etc/fstab "$TARGET_HOME/.claude/CLAUDE.md"; do
    sauver "$p"
done
info "sauvegarde : $BACKUP"

#==============================================================================
# LES PROGRAMMES
#==============================================================================
note "Guichet, commande d'administration, fenetre de confirmation"

run install -D -m 0755 "$REPO_DIR/rootfs/usr/local/bin/claude-os-root"    "$GUICHET"
run install -D -m 0755 "$REPO_DIR/rootfs/usr/local/bin/claude-os"         /usr/local/bin/claude-os
run install -D -m 0755 "$REPO_DIR/rootfs/usr/local/bin/claude-os-askpass" /usr/local/bin/claude-os-askpass
good "trois programmes installes dans /usr/local/bin"

# La porte du niveau 3. C'est le MEME fichier, sous un autre chemin -- et
# c'est le chemin, pas le fichier, que sudoers reconnait. Absent de la regle
# NOPASSWD, ce chemin reclame donc un mot de passe, ce qui est exactement la
# confirmation humaine du niveau 3. Verifie : sudo traite un lien symbolique
# comme une commande distincte, il ne le suit pas jusqu'a sa cible.
run ln -sfn "$GUICHET" "$SENSIBLE"
good "porte de niveau 3 : $SENSIBLE -> $(basename "$GUICHET")"

run install -D -m 0644 "$REPO_DIR/rootfs/usr/share/claude-os/politique/politique.conf" \
    "$REF/politique.conf"
run install -D -m 0644 "$REPO_DIR/rootfs/usr/share/claude-os/politique/instructions-claude.md" \
    "$REF/instructions-claude.md"

# La politique qui fait foi n'est ecrite QU'UNE FOIS. Une mise a jour du
# depot ne doit pas balayer un reglage local -- meme raison que pour
# ~/.config/claude-os/shell.conf.
run mkdir -p /etc/claude-os
if [[ -f /etc/claude-os/politique.conf ]]; then
    info "/etc/claude-os/politique.conf existe : conserve tel quel"
else
    run cp -a "$REF/politique.conf" /etc/claude-os/politique.conf
    good "/etc/claude-os/politique.conf ecrit (valeurs par defaut)"
fi

#==============================================================================
# JOURNAL
#==============================================================================
note "Journal des actions privilegiees"

run mkdir -p "$JOURNAL_DIR"
run chmod 0750 "$JOURNAL_DIR"
if [[ ! -f "$JOURNAL_DIR/actions.log" ]]; then
    run touch "$JOURNAL_DIR/actions.log"
fi
run chmod 0640 "$JOURNAL_DIR/actions.log"
# Le groupe du journal est celui du guichet, et non « adm » : « claude-os
# journal » doit se lire depuis le compte du bureau sans passer par sudo.
# En lecture seule -- l'ecriture reste au guichet, qui est root.
run chgrp "$GROUPE" "$JOURNAL_DIR" 2>/dev/null || true
run chgrp "$GROUPE" "$JOURNAL_DIR/actions.log" 2>/dev/null || true

# Le journal est double dans journald (« journalctl -t claude-os-root »), qui
# est l'exemplaire difficile a retoucher. Le fichier, lui, est celui qu'on lit
# -- et qui doit donc tourner, sinon il finit par remplir un eMMC de 64 Go.
ecrire /etc/logrotate.d/claude-os <<'ROT'
/var/log/claude-os/actions.log {
    monthly
    rotate 12
    compress
    delaycompress
    missingok
    notifempty
    create 0640 root claude-os-root
}
ROT
good "journal : $JOURNAL_DIR/actions.log (rotation mensuelle, 12 mois)"

#==============================================================================
# INSTANTANES BTRFS
#==============================================================================
note "Instantanes"

FS_RACINE="$(stat -f -c %T / 2>/dev/null || echo inconnu)"
if [[ "$FS_RACINE" != "btrfs" ]]; then
    warn "la racine est en « $FS_RACINE », pas en btrfs."
    warn "Les ecritures de niveau 2 s'executeront, mais ne seront PAS annulables."
    warn "C'est une perte de fonction, pas un echec : le journal reste tenu."
elif ((SNAPSHOTS == 0)); then
    info "--sans-btrfs : depot d'instantanes laisse en l'etat"
elif findmnt -n /.snapshots >/dev/null 2>&1; then
    good "/.snapshots deja monte"
else
    SUBVOL="$(findmnt -no FSROOT / | sed 's#^/##')"
    UUID="$(findmnt -no UUID /)"
    DEV="$(findmnt -no SOURCE / | sed 's/\[.*//')"
    if [[ "$SUBVOL" != "@" ]]; then
        warn "la racine est le sous-volume « ${SUBVOL:-/} » et non « @ »."
        warn "Le depot @snapshots n'est pas cree automatiquement sur une"
        warn "disposition inconnue. Repli : $(grep -m1 '^instantanes_dir' "$REF/politique.conf")"
    elif [[ -z "$UUID" ]]; then
        warn "UUID de la racine illisible : entree fstab non ecrite."
    else
        info "racine : $DEV (UUID=$UUID), sous-volume @"
        POINT="$(mktemp -d /run/claude-os-patch.XXXXXX)"
        if ((DRY)); then
            info "creation du sous-volume @snapshots, puis montage sur /.snapshots"
            rmdir "$POINT"
        else
            mount -o subvolid=5 "$DEV" "$POINT" || die "montage de la racine btrfs impossible."
            if [[ ! -d "$POINT/@snapshots" ]]; then
                btrfs subvolume create "$POINT/@snapshots" >/dev/null \
                    || { umount "$POINT"; rmdir "$POINT"; die "creation de @snapshots impossible."; }
                good "sous-volume @snapshots cree"
            else
                info "sous-volume @snapshots deja present"
            fi
            umount "$POINT"; rmdir "$POINT"
        fi

        LIGNE="UUID=$UUID /.snapshots btrfs noatime,ssd,discard=async,compress=zstd:1,subvol=@snapshots 0 0"
        if grep -q '[[:space:]]/\.snapshots[[:space:]]' /etc/fstab; then
            info "/etc/fstab mentionne deja /.snapshots"
        else
            if ((DRY)); then
                info "ajout dans /etc/fstab : $LIGNE"
            else
                printf '\n# Claude OS : depot des instantanes du guichet root (claude-os-root)\n%s\n' \
                    "$LIGNE" >> /etc/fstab
            fi
            good "entree fstab ajoutee"
        fi
        run mkdir -p /.snapshots
        if ((DRY == 0)); then
            systemctl daemon-reload >/dev/null 2>&1 || true
            if mount /.snapshots 2>/dev/null && findmnt -n /.snapshots >/dev/null 2>&1; then
                good "/.snapshots monte"
            else
                # Une entree fstab fausse empeche le demarrage. On remet la
                # sauvegarde plutot que de retoucher le fichier a la sed :
                # elle date de quelques secondes et elle est juste.
                warn "le montage de /.snapshots a echoue."
                if [[ -f "$BACKUP/etc/fstab" ]]; then
                    cp -a "$BACKUP/etc/fstab" /etc/fstab
                    warn "/etc/fstab restaure depuis $BACKUP/etc/fstab"
                fi
                warn "Les instantanes se replieront sur $REF/politique.conf (instantanes_dir)."
            fi
        fi
    fi
fi

#==============================================================================
# LA REGLE SUDO
#==============================================================================
note "Regle sudo"

if id -nG "$TARGET_USER" | tr ' ' '\n' | grep -qx "$GROUPE"; then
    info "$TARGET_USER est deja membre de $GROUPE"
else
    run usermod -aG "$GROUPE" "$TARGET_USER"
    good "$TARGET_USER ajoute au groupe $GROUPE"
    RECONNEXION=1
fi

# La regle est ECRITE A PART, VERIFIEE, puis seulement installee. Un fichier
# invalide dans /etc/sudoers.d casse sudo pour TOUT LE MONDE, y compris pour
# la commande qui servirait a le reparer.
TMPREGLE="$(mktemp)"
cat > "$TMPREGLE" <<REGLE_EOF
# Claude OS — acces root de Claude Desktop.
#
# Ecrit par install/patch-acces-root-claude.sh. Ne pas editer a la main :
# une erreur ici casse sudo pour tout le systeme.
#
# UNE SEULE commande passe sans mot de passe, et c'est le guichet. Tout le
# reste — y compris « sudo apt install » — reste soumis au mot de passe, et
# donc a la confirmation humaine. C'est la difference entre deleguer un
# pouvoir et donner les cles.
#
# L'ORDRE DES DEUX REGLES COMPTE : sudoers retient la DERNIERE qui
# correspond. Inverser ces lignes rendrait tout gratuit.

# 1. Le droit general, AVEC mot de passe. Sans lui, le niveau 3 serait
#    impossible : le compte n'aurait tout simplement pas le droit de lancer
#    la commande, meme en connaissant le mot de passe. Sur une Debian
#    installee avec un mot de passe root, le premier compte n'est pas dans le
#    groupe « sudo » -- il faut donc l'accorder ici, explicitement.
%$GROUPE ALL=(ALL:ALL) ALL

# 2. Le guichet, sans mot de passe. Un Cmnd sans argument accepte tous les
#    arguments : c'est le guichet qui classe la commande, pas sudoers, qui
#    n'a pas les moyens de le faire correctement.
#
#    $SENSIBLE n'est PAS ici, et c'est tout le mecanisme : le meme programme,
#    appele par ce second chemin, retombe sur la regle 1 et reclame le mot de
#    passe. C'est ainsi que le niveau 3 est impose par sudo, et non par la
#    bonne volonte du programme.
%$GROUPE ALL=(root) NOPASSWD: $GUICHET

# Ce qui suit s'applique aux commandes de NIVEAU 3, celles que le guichet
# refuse et qu'on relance sous sudo.
#
# Le programme d'invite lui-meme n'est PAS declare ici : « askpass » est un
# reglage de /etc/sudo.conf, pas de sudoers, et le mettre ici ferait rejeter
# tout le fichier (« unknown defaults entry »). Il est ecrit par le meme
# script, quelques lignes plus loin.

# La fenetre ne peut s'ouvrir que si la session est joignable.
Defaults:%$GROUPE env_keep += "WAYLAND_DISPLAY XDG_RUNTIME_DIR XDG_SESSION_TYPE"

# Aucune mise en cache de l'autorisation : chaque action sensible est
# approuvee pour elle-meme. C'est le sens de « confirmation humaine
# explicite » — une approbation ne doit pas en couvrir quinze autres pendant
# le quart d'heure qui suit.
Defaults:%$GROUPE timestamp_timeout=0

# Une seule tentative : la fenetre est fermee des le refus, plutot que de
# reapparaitre deux fois de plus.
Defaults:%$GROUPE passwd_tries=1
REGLE_EOF

if ! visudo -cqf "$TMPREGLE"; then
    rm -f "$TMPREGLE"
    die "la regle sudo produite est invalide. Rien n'a ete installe."
fi
info "regle verifiee par visudo"

if ((DRY)); then
    printf '   \033[2m$ install -m 0440 -o root -g root <regle> %s\033[0m\n' "$REGLE"
    sed 's/^/   \x1b[2m|\x1b[0m /' "$TMPREGLE"
    rm -f "$TMPREGLE"
else
    install -m 0440 -o root -g root "$TMPREGLE" "$REGLE"
    rm -f "$TMPREGLE"
    # Deuxieme controle : cette fois sur l'ENSEMBLE de la configuration sudo,
    # telle qu'elle sera lue au prochain appel.
    if ! visudo -cq; then
        rm -f "$REGLE"
        die "la configuration sudo complete est invalide : la regle a ete retiree."
    fi
    good "regle installee : $REGLE (0440)"
fi

#==============================================================================
# FENETRE DE CONFIRMATION (sudo.conf)
#==============================================================================
note "Programme d'invite de sudo"

# sudo n'apprend l'existence d'un programme d'invite que par /etc/sudo.conf.
# Sans cette ligne, une commande de niveau 3 lancee depuis Claude Desktop —
# donc sans terminal — echoue sur « sudo: no tty present and no askpass
# program specified », et personne ne voit jamais la demande.
LIGNE_ASKPASS="Path askpass /usr/local/bin/claude-os-askpass"
sauver /etc/sudo.conf
if [[ -f /etc/sudo.conf ]] && grep -qE '^[[:space:]]*Path[[:space:]]+askpass[[:space:]]' /etc/sudo.conf; then
    ACTUEL="$(grep -E '^[[:space:]]*Path[[:space:]]+askpass[[:space:]]' /etc/sudo.conf | head -n1)"
    if [[ "$ACTUEL" == "$LIGNE_ASKPASS" ]]; then
        info "/etc/sudo.conf designe deja claude-os-askpass"
    else
        warn "un autre programme d'invite est declare : $ACTUEL"
        if ((DRY)); then
            info "il serait remplace par : $LIGNE_ASKPASS"
        else
            sed -i "s|^[[:space:]]*Path[[:space:]]\+askpass[[:space:]].*|$LIGNE_ASKPASS|" /etc/sudo.conf
            good "/etc/sudo.conf mis a jour (ancien fichier dans $BACKUP)"
        fi
    fi
elif ((DRY)); then
    info "ajout dans /etc/sudo.conf : $LIGNE_ASKPASS"
else
    printf '\n# Claude OS : confirmation humaine des actions de niveau 3 (docs/07).\n%s\n' \
        "$LIGNE_ASKPASS" >> /etc/sudo.conf
    good "/etc/sudo.conf : $LIGNE_ASKPASS"
fi

#==============================================================================
# INSTRUCTIONS POUR CLAUDE DESKTOP
#==============================================================================
note "Instructions du compte"

# C'est ce qui rend l'acces NATIF plutot que disponible : sans cela, Claude
# tente « sudo apt install », se heurte a une demande de mot de passe qu'il ne
# peut pas satisfaire, et conclut qu'il n'a pas les droits. Le guichet doit
# etre annonce la ou l'application lit ses consignes.
CLAUDE_DIR="$TARGET_HOME/.claude"
CLAUDE_MD="$CLAUDE_DIR/CLAUDE.md"
DEBUT='<!-- claude-os:debut — bloc gere par install/patch-acces-root-claude.sh -->'
FIN_B='<!-- claude-os:fin -->'

if ((DRY)); then
    info "bloc « claude-os » ecrit ou mis a jour dans $CLAUDE_MD"
else
    mkdir -p "$CLAUDE_DIR"
    [[ -f "$CLAUDE_MD" ]] || : > "$CLAUDE_MD"
    TMP="$(mktemp)"
    # Le contenu personnel du fichier est preserve : on ne remplace que le
    # bloc delimite, et on l'ajoute a la fin s'il n'y est pas encore.
    awk -v d="$DEBUT" -v f="$FIN_B" '
        index($0, "claude-os:debut") {s=1; next}
        index($0, "claude-os:fin")   {s=0; next}
        !s {print}
    ' "$CLAUDE_MD" > "$TMP"
    {
        cat "$TMP"
        printf '\n%s\n\n' "$DEBUT"
        cat "$REF/instructions-claude.md"
        printf '\n%s\n' "$FIN_B"
    } > "$CLAUDE_MD"
    rm -f "$TMP"
    chown -R "$TARGET_USER": "$CLAUDE_DIR"
    good "bloc « claude-os » a jour dans ~/.claude/CLAUDE.md"
fi

#==============================================================================
# CONTROLE
#==============================================================================
note "Controle"

if ((DRY)); then
    info "simulation : le controle reel demande une installation effective."
    echo
    info "Rien n'a ete modifie. Relancer sans --dry-run pour appliquer."
    exit 0
fi

ECHECS=0
verif() {
    if eval "$2" >/dev/null 2>&1; then good "$1"; else printf '   \033[31m- %s\033[0m\n' "$1"; ECHECS=$((ECHECS+1)); fi
}
verif "guichet executable"                  "[ -x $GUICHET ]"
verif "porte de niveau 3 en place"          "[ -L $SENSIBLE ]"
verif "commande claude-os executable"       "[ -x /usr/local/bin/claude-os ]"
verif "fenetre de confirmation executable"  "[ -x /usr/local/bin/claude-os-askpass ]"
verif "regle sudo en 0440"                  "[ \"\$(stat -c %a $REGLE)\" = 440 ]"
verif "configuration sudo valide"           "visudo -cq"
verif "groupe $GROUPE present"              "getent group $GROUPE"
verif "$TARGET_USER membre du groupe"       "id -nG $TARGET_USER | tr ' ' '\n' | grep -qx $GROUPE"
verif "sudo.conf designe l'invite"      "grep -qE '^Path askpass /usr/local/bin/claude-os-askpass$' /etc/sudo.conf"
verif "politique lisible"                   "[ -r /etc/claude-os/politique.conf ]"
verif "journal accessible"                  "[ -w $JOURNAL_DIR ]"

# Le seul controle qui compte vraiment : le compte du bureau peut-il
# atteindre le guichet sans mot de passe ? On le demande a sudo lui-meme.
# On EXECUTE, on ne se contente pas de « sudo -l » : lister dit seulement que
# la commande est permise, pas qu'elle passe sans mot de passe. Les deux
# portes doivent se comporter differemment, et c'est cela qu'on mesure.
# « id -u » est de niveau 1 : rien n'est ecrit, aucun instantane n'est pris.
if runuser -u "$TARGET_USER" -- sudo -n -- "$GUICHET" id -u >/dev/null 2>&1; then
    good "$TARGET_USER atteint le guichet sans mot de passe"
else
    warn "sudo ne laisse pas encore passer $TARGET_USER par le guichet."
    warn "C'est attendu juste apres l'ajout au groupe : l'appartenance ne"
    warn "prend effet qu'a la prochaine ouverture de session. Se reconnecter,"
    warn "puis relancer « claude-os etat »."
fi

# Le controle symetrique, et le plus important des deux : la porte du niveau 3
# doit, elle, etre REFUSEE tant qu'aucun mot de passe n'a ete donne.
if runuser -u "$TARGET_USER" -- sudo -n -- "$SENSIBLE" id -u >/dev/null 2>&1; then
    printf '   \033[31m- la porte de niveau 3 passe SANS mot de passe : elle ne protege rien.\033[0m\n'
    printf '   \033[31m  Verifier que %s n'"'"'est pas couvert par une autre regle NOPASSWD.\033[0m\n' "$SENSIBLE"
    ECHECS=$((ECHECS+1))
else
    good "la porte de niveau 3 reclame bien un mot de passe"
fi

# Un essai de bout en bout, en niveau 1 : rien n'est ecrit, mais tout le
# chemin est parcouru -- classement, journal, execution.
if "$GUICHET" --pourquoi "controle du correctif" id -u >/dev/null 2>&1; then
    good "essai de bout en bout : le guichet execute et journalise"
else
    warn "l'essai du guichet a echoue -- voir « claude-os etat »"
    ECHECS=$((ECHECS+1))
fi

#==============================================================================
echo
if ((ECHECS == 0)); then
    printf '\033[32m   Acces root en place.\033[0m\n'
else
    printf '\033[31m   %d controle(s) en echec.\033[0m Sauvegarde : %s\n' "$ECHECS" "$BACKUP"
fi
echo
info "A faire maintenant :"
if [[ -n "${RECONNEXION:-}" ]]; then
    info "  1. Fermer la session et la rouvrir (Super + Maj + Q), pour que"
    info "     l'appartenance au groupe $GROUPE prenne effet."
    info "  2. Dans Claude Desktop :  claude-os etat"
else
    info "  Dans Claude Desktop :  claude-os etat"
fi
echo
info "Puis, pour verifier le classement sans rien executer :"
info "  claude-os-root --expliquer apt-get install tree      -> niveau 2"
info "  claude-os-root --expliquer journalctl -b -p err      -> niveau 1"
info "  claude-os-root --expliquer parted /dev/mmcblk0 print -> niveau 3"
echo
info "Le niveau 3 ne se refuse plus : il ouvre une fenetre sur la session,"
info "montre la commande, et attend le mot de passe. A essayer depuis le"
info "bureau (pas depuis SSH) :  claude-os-root passwd $TARGET_USER"
echo
info "Sauvegarde de l'etat precedent : $BACKUP"
info "Pour tout defaire :  sudo bash install/patch-acces-root-claude.sh --retirer"
echo
