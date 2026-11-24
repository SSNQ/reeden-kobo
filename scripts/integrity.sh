#!/bin/sh
# Verify every shipped file; restore only missing resource files from a checked
# locally assembled archive. Never rewrite SHA256SUMS or replace changed files.
set -u
set -f
APP=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODE=${1:-check}
case "$MODE" in check|repair) :;; *) echo 'Usage: integrity.sh check|repair' >&2; exit 64;; esac
MANIFEST="$APP/SHA256SUMS"
ARCHIVE_REL=integrity/resources.tar.gz
ARCHIVE="$APP/$ARCHIVE_REL"
TMPFILE=
cleanup() { [ -z "$TMPFILE" ] || rm -f "$TMPFILE"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
fail() { printf '[INTEGRITY] %s\n' "$*" >&2; exit 1; }
[ -f "$MANIFEST" ] && [ ! -L "$MANIFEST" ] || fail 'SHA256SUMS is missing or not a regular manifest'

if [ "$MODE" = repair ]; then
    [ ! -d "$APP/state/active" ] || fail 'Exit Reeden before resource repair'
    missing_resources=0
    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || continue
        digest=${line%%  *}
        rel=${line#*  }
        [ "$rel" != "$line" ] && [ "${#digest}" -eq 64 ] || fail 'Invalid checksum manifest line'
        case "$digest" in *[!0-9a-f]*) fail 'Invalid checksum digest';; esac
        case "$rel" in core/data/*|ui-assets/*) :;; *) continue;; esac
        case "$rel" in *'/../'*|*'/..'|*'/./'*|*'//'*) fail "Unsafe resource path: $rel";; esac
        [ ! -L "$APP/$rel" ] || fail "Resource is a symlink: $rel"
        [ -e "$APP/$rel" ] || { missing_resources=1; break; }
    done < "$MANIFEST"
    if [ "$missing_resources" = 1 ]; then
    # Validate the archive before opening any resource for writing. The archive
    # is private to this installed package, not downloaded during device startup.
    expected=$(awk '$2 == "integrity/resources.tar.gz" {print $1; n++} END {if (n != 1) exit 1}' "$MANIFEST") || fail 'Recovery archive has no unique checksum entry'
    [ "${#expected}" -eq 64 ] || fail 'Invalid recovery archive checksum'
    case "$expected" in *[!0-9a-f]*) fail 'Invalid recovery archive checksum';; esac
    [ -f "$ARCHIVE" ] && [ ! -L "$ARCHIVE" ] || fail 'Recovery archive missing; reinstall using the computer installer'
    actual=$(sha256sum "$ARCHIVE") || fail 'Cannot read recovery archive'
    actual=${actual%% *}
    [ "$actual" = "$expected" ] || fail 'Recovery archive checksum failed; existing files were not changed'

    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || continue
        digest=${line%%  *}
        rel=${line#*  }
        [ "$rel" != "$line" ] && [ "${#digest}" -eq 64 ] || fail 'Invalid checksum manifest line'
        case "$digest" in *[!0-9a-f]*) fail 'Invalid checksum digest';; esac
        # The archive can repair resource bytes only, never binaries, scripts,
        # user data, settings, the manifest, or the archive itself.
        case "$rel" in core/data/*|ui-assets/*) :;; *) continue;; esac
        case "$rel" in *'/../'*|*'/..'|*'/./'*|*'//'*) fail "Unsafe resource path: $rel";; esac
        [ ! -L "$APP/$rel" ] || fail "Resource is a symlink: $rel"
        [ ! -e "$APP/$rel" ] || continue
        parent="$APP"
        remaining="$rel"
        while [ "$remaining" != "${remaining#*/}" ]; do
            part=${remaining%%/*}
            remaining=${remaining#*/}
            parent="$parent/$part"
            [ ! -L "$parent" ] || fail "Resource parent is a symlink: $rel"
            if [ -e "$parent" ] && [ ! -d "$parent" ]; then
                fail "Resource parent is not a directory: $rel"
            fi
        done
        mkdir -p "$parent" || fail "Cannot create resource directory: $rel"
        TMPFILE=$(mktemp "$parent/.reeden-resource-XXXXXX") || fail "Cannot stage resource: $rel"
        if ! tar -xzOf "$ARCHIVE" "$rel" > "$TMPFILE"; then
            fail "Cannot extract missing resource: $rel"
        fi
        actual=$(sha256sum "$TMPFILE") || fail "Cannot hash staged resource: $rel"
        actual=${actual%% *}
        [ "$actual" = "$digest" ] || fail "Recovered resource hash mismatch: $rel"
        chmod 644 "$TMPFILE" || fail "Cannot set resource permissions: $rel"
        # POSIX noclobber creates the destination exclusively (O_EXCL). VFAT
        # has no hard links, and plain mv/mv -n can race between stat and rename.
        # A failed or interrupted write still fails closed on the full checksum.
        if ! (set -C; umask 022; cat "$TMPFILE" > "$APP/$rel"); then
            fail "Cannot create missing resource exclusively: $rel"
        fi
        rm -f "$TMPFILE"
        TMPFILE=
        printf '[RESTORED] %s\n' "$rel"
    done < "$MANIFEST"
    fi
fi

# A historical install-verified marker is never a replacement for this check.
# Keep the original checker output, including every missing or changed path.
cd "$APP" || exit 1
sha256sum -c SHA256SUMS
