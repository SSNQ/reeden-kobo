#!/bin/sh
# Unpack under .adds/.reeden-kobo-stage-*; commit only after copying user data.
set -eu
UPGRADE=0
if [ "${1:-}" = "--upgrade" ]; then UPGRADE=1; shift; fi
SOURCE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=/mnt/onboard/.adds/reeden-kobo
ENTRY=/mnt/onboard/.adds/nm/reeden-kobo
case "$SOURCE" in /mnt/onboard/.adds/.reeden-kobo-stage-*) :;; *) echo 'Unpack to an .adds/.reeden-kobo-stage-* directory first.'; exit 64;; esac
if [ -e "$DEST" ]; then
    [ "$UPGRADE" -eq 1 ] || { echo 'Existing installation preserved. Pass --upgrade after exiting Reeden.'; exit 1; }
    [ ! -d "$DEST/state/active" ] || { echo 'Reeden is currently active. Exit Reeden before upgrading.'; exit 1; }
fi
[ -d /mnt/onboard/.adds/nm ] || { echo 'NickelMenu is required; no system changes were made.'; exit 1; }
if [ -e "$ENTRY" ] && [ "$UPGRADE" -ne 1 ]; then
    echo 'Existing menu file preserved; pass --upgrade after checking it.'; exit 1
fi
product=$(awk -F, '{print substr($NF,length($NF)-2)}' /mnt/onboard/.kobo/version)
[ "$product" = 393 ] || { echo 'Only Kobo Clara Colour product 393 is supported.'; exit 1; }
cd "$SOURCE"
sha256sum -c SHA256SUMS > install-verification.log 2>&1 || { echo 'Checksum failed; see install-verification.log'; exit 1; }
chmod 755 ./*.sh ./bin/* ./runtime/* ./checks/*
./bin/fb-guard --inspect || { echo 'Wrong framebuffer/model; installation aborted.'; exit 1; }
for file in ./*.sh ./bin/lipc-get-prop ./bin/lipc-set-prop; do sh -n "$file"; done
BACKUP=""
OLD_MOVED=0
PROMOTED=0
COMMITTED=0
MENU_STAGE=$(mktemp /mnt/onboard/.adds/nm/.reeden-kobo-menu-XXXXXX)
finish_install() {
    result=$?
    trap - EXIT HUP INT TERM
    set +e
    if [ "$COMMITTED" -eq 0 ]; then
        rollback_ok=1
        if [ "$PROMOTED" -eq 1 ]; then mv "$DEST" "$SOURCE" || rollback_ok=0; fi
        if [ "$OLD_MOVED" -eq 1 ] && [ "$rollback_ok" -eq 1 ]; then mv "$BACKUP" "$DEST" || rollback_ok=0; fi
        if [ "$rollback_ok" -eq 0 ]; then
            echo "ROLLBACK FAILED: preserve $BACKUP, $DEST and $SOURCE for manual recovery." >&2
            result=1
        fi
    fi
    rm -f "$MENU_STAGE"
    exit "$result"
}
trap finish_install EXIT
trap 'exit 143' HUP INT TERM
# Copy while the original installation is still launchable. A failed copy
# cannot remove its menu target. Keep the backup after a successful commit.
if [ -e "$DEST" ]; then
    for keep in userdata storage diagnostics/ssh; do
        if [ -d "$DEST/$keep" ]; then
            mkdir -p "$SOURCE/$keep"
            cp -R "$DEST/$keep/." "$SOURCE/$keep/" || { echo "Failed copying $keep; original installation preserved." >&2; exit 1; }
        fi
    done
fi
cp "$SOURCE/nickelmenu.conf" "$MENU_STAGE"
[ ! -d "$DEST/state/active" ] || { echo 'Reeden became active; installation cancelled.'; exit 1; }
if [ -e "$DEST" ]; then
    BACKUP=$(mktemp -d /mnt/onboard/.adds/reeden-kobo-backup-XXXXXX)
    rmdir "$BACKUP"
    mv "$DEST" "$BACKUP"
    OLD_MOVED=1
fi
mv "$SOURCE" "$DEST"
PROMOTED=1
keep_menu=0
if [ -L "$ENTRY" ] || { [ -e "$ENTRY" ] && [ ! -f "$ENTRY" ]; }; then
    echo 'Menu is not a regular file; refusing to replace it.'; exit 1
elif [ -f "$ENTRY" ]; then
    if [ -z "$BACKUP" ] || [ ! -f "$BACKUP/nickelmenu.conf" ] || [ -L "$BACKUP/nickelmenu.conf" ]; then
        keep_menu=1
    elif cmp -s "$ENTRY" "$BACKUP/nickelmenu.conf"; then :
    else
        result=$?
        [ "$result" -eq 1 ] || { echo 'Menu comparison failed; keeping previous installation.'; exit 1; }
        keep_menu=1
    fi
fi
if [ "$keep_menu" -eq 1 ]; then
    echo "Custom menu retained; new default is $DEST/nickelmenu.conf."
else
    mv -f "$MENU_STAGE" "$ENTRY"
fi
COMMITTED=1
sync
if [ -n "$BACKUP" ]; then
    echo "Upgraded $DEST (previous build backed up to $BACKUP, userdata and storage preserved); FIRST LAUNCH WAS NOT PERFORMED."
else
    echo "Installed to $DEST; FIRST LAUNCH WAS NOT PERFORMED."
fi
