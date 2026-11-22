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
KOBO_CONF_DIR=/mnt/onboard/.kobo/Kobo
KOBO_CONF="$KOBO_CONF_DIR/Kobo eReader.conf"
[ ! -L /mnt/onboard/.kobo ] && [ ! -L "$KOBO_CONF_DIR" ] && [ ! -L "$KOBO_CONF" ] || { echo 'Unsafe symlink at Kobo configuration path; installation aborted.'; exit 1; }
if [ -e "$KOBO_CONF_DIR" ] && [ ! -d "$KOBO_CONF_DIR" ]; then echo 'Invalid Kobo configuration directory; installation aborted.'; exit 1; fi
if [ -e "$KOBO_CONF" ] && [ ! -f "$KOBO_CONF" ]; then echo 'Invalid Kobo configuration file; installation aborted.'; exit 1; fi
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
mv -f "$MENU_STAGE" "$ENTRY"
COMMITTED=1
ensure_kobo_exclude_sync_folders() {
    rule='(\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+)'
    inner='\\.(?!kobo|adobe).+|([^.][^/]*/)+\\..+'
    mkdir -p "$KOBO_CONF_DIR" || return 1
    conf_stage=$(mktemp "$KOBO_CONF_DIR/.Kobo-eReader.conf.reeden-XXXXXX") || return 1
    if [ ! -f "$KOBO_CONF" ]; then
        printf '[FeatureSettings]\nExcludeSyncFolders=%s\n' "$rule" > "$conf_stage" || { rm -f "$conf_stage"; return 1; }
        mv "$conf_stage" "$KOBO_CONF" || { rm -f "$conf_stage"; return 1; }
        echo "Configured ExcludeSyncFolders in $KOBO_CONF (added)."
        return 0
    fi
    exclude_status=$(REEDEN_EXCLUDE_RULE="$rule" REEDEN_EXCLUDE_INNER="$inner" REEDEN_CONF_OUT="$conf_stage" \
        REEDEN_MATCH_KOBO='\\.(?!kobo|adobe)' REEDEN_MATCH_ADDS='\\.adds' \
        awk '
        function trim(s) {
            sub(/^[ \t]+/, "", s)
            sub(/[ \t]+$/, "", s)
            return s
        }
        {
            sub(/\r$/, "")
            lines[NR] = $0
            t = trim($0)
            if (substr(t, 1, 1) == "[" && substr(t, length(t), 1) == "]" && length(t) >= 2) {
                if (in_fs) { fs_end[fs_count] = NR - 1; in_fs = 0 }
                sec = trim(substr(t, 2, length(t) - 2))
                if (sec == "FeatureSettings") { fs_count++; fs_start[fs_count] = NR; in_fs = 1 }
                next
            }
            if (in_fs && $0 ~ /^[ \t]*ExcludeSyncFolders[ \t]*=/) {
                eq = index($0, "=")
                ex_line = NR
                ex_val = substr($0, eq + 1)
            }
        }
        END {
            if (in_fs) { fs_end[fs_count] = NR }
            rule = ENVIRON["REEDEN_EXCLUDE_RULE"]
            inner = ENVIRON["REEDEN_EXCLUDE_INNER"]
            out = ENVIRON["REEDEN_CONF_OUT"]
            m_kobo = ENVIRON["REEDEN_MATCH_KOBO"]
            m_adds = ENVIRON["REEDEN_MATCH_ADDS"]
            status = ""
            insert_after = -1
            if (ex_line > 0) {
                v = trim(ex_val)
                if (v == "") {
                    lines[ex_line] = "ExcludeSyncFolders=" rule
                    status = "updated"
                } else if (index(v, m_kobo) > 0 || index(v, m_adds) > 0) {
                    status = "already_configured"
                } else if (substr(v, 1, 1) == "\"" || substr(v, length(v), 1) == "\"") {
                    status = "manual_required"
                } else {
                    lines[ex_line] = "ExcludeSyncFolders=((" v ")|" inner ")"
                    status = "updated"
                }
            } else if (fs_count > 0) {
                idx = fs_end[fs_count]
                while (idx > fs_start[fs_count] && trim(lines[idx]) == "") { idx-- }
                insert_after = idx
                status = "added"
            } else {
                status = "append_section"
            }
            if (status == "already_configured" || status == "manual_required") {
                print status
                exit 0
            }
            if (status == "append_section") {
                for (i = 1; i <= NR; i++) { print lines[i] > out }
                if (NR > 0 && trim(lines[NR]) != "") { print "" > out }
                print "[FeatureSettings]" > out
                print "ExcludeSyncFolders=" rule > out
                close(out)
                print "added"
                exit 0
            }
            for (i = 1; i <= NR; i++) {
                print lines[i] > out
                if (i == insert_after) { print "ExcludeSyncFolders=" rule > out }
            }
            close(out)
            print status
        }
    ' "$KOBO_CONF") || { rm -f "$conf_stage"; return 1; }
    case "$exclude_status" in
        added|updated)
            mv "$conf_stage" "$KOBO_CONF" || { rm -f "$conf_stage"; return 1; }
            echo "Configured ExcludeSyncFolders in $KOBO_CONF ($exclude_status)."
            ;;
        already_configured)
            rm -f "$conf_stage"
            echo "ExcludeSyncFolders already excludes .adds in $KOBO_CONF; kept unchanged."
            ;;
        *)
            rm -f "$conf_stage"
            echo "Warning: custom ExcludeSyncFolders in $KOBO_CONF was not modified automatically; ensure .adds is excluded." >&2
            ;;
    esac
}
ensure_kobo_exclude_sync_folders || echo "Warning: could not update $KOBO_CONF automatically." >&2
sync
if [ -n "$BACKUP" ]; then
    echo "Upgraded $DEST (previous build backed up to $BACKUP, userdata and storage preserved); FIRST LAUNCH WAS NOT PERFORMED."
else
    echo "Installed to $DEST; FIRST LAUNCH WAS NOT PERFORMED."
fi
