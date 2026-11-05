#!/bin/sh
set -u
. "$(dirname -- "$0")/session.sh"
product=$(awk -F, '{print substr($NF,length($NF)-2)}' /mnt/onboard/.kobo/version 2>/dev/null)
[ "$product" = 393 ] || { echo 'Only Kobo Clara Colour product 393 is supported; launch cancelled.'; exit 12; }
mkdir -p "$APP/logs" "$STATE" "$APP/cache" "$APP/userdata" "$APP/storage/books" "$APP/storage/documents" "$APP/storage/screenshots" "$APP/userdata/.config" "$APP/userdata/.local/share" "$APP/userdata/.cache"
exec >> "$APP/logs/launcher.log" 2>&1
echo "[launcher] $(date)"
if [ -d "$STATE/active" ]; then
    if [ -f "$STATE/owner" ]; then read -r pid start < "$STATE/owner"; if same_process "$pid" "$start"; then echo "Already running"; exit 0; fi; fi
    "$APP/recover.sh"
    [ ! -d "$STATE/active" ] || { echo "Previous session recovery is busy or failed; see launcher.log"; exit 14; }
fi
# Never take display ownership from KOReader. Return to Nickel first.
for pid in $(pidof luajit 2>/dev/null); do
    if tr '\000' ' ' < "/proc/$pid/cmdline" | grep -q reader.lua; then echo "Exit KOReader and launch from NickelMenu"; exit 10; fi
done
NICKEL=$(pidof nickel 2>/dev/null) || { echo "Nickel is not running; refusing to take ownership"; exit 11; }
chmod 755 "$APP"/*.sh "$APP"/bin/* "$APP"/runtime/* "$APP"/checks/* 2>/dev/null || true
if [ -f "$APP/SHA256SUMS" ] && [ ! -f "$STATE/install-verified" ]; then
    (cd "$APP" && sha256sum -c SHA256SUMS > "$APP/install-verification.log" 2>&1) || { echo "Package checksum verification failed; see install-verification.log"; exit 12; }
    : > "$STATE/install-verified"
fi
"$APP/bin/fb-guard" --inspect || { echo "Unsupported framebuffer configuration"; exit 12; }
"$APP/runtime/reeden-kindle" --verify "$APP/bin/reeden-kindle" || exit 13
mkdir "$STATE/active" || exit 14
echo "$$ $(token $$)" > "$STATE/owner"
rm -f "$STATE/host" "$STATE/heartbeat" "$STATE/ready" "$STATE/handoff" "$STATE/asleep" "$STATE/power-cancel" "$STATE/power-phase" "$STATE/power-phase.new"
: > "$STATE/paused"
if [ -f "$APP/logs/current.log" ]; then mv -f "$APP/logs/current.log" "$APP/logs/previous.log"; fi
: > "$APP/logs/current.log"
trap '"$APP/recover.sh"' EXIT
trap 'exit 143' TERM HUP INT
nohup "$APP/guardian.sh" > "$APP/logs/guardian.log" 2>&1 < /dev/null &
read -r light < /sys/class/backlight/mxc_msp430.0/brightness
read -r warm < /sys/class/backlight/lm3630a_led/color
echo "$light $warm" > "$STATE/light"
export REEDEN_KOBO_DIR="$APP"
export REEDEN_BUNDLE_DIR="$APP/core"
export REEDEN_ASSETS_DIR="$APP/ui-assets"
export HOME="$APP/userdata" XDG_CONFIG_HOME="$APP/userdata/.config" XDG_DATA_HOME="$APP/userdata/.local/share" XDG_CACHE_HOME="$APP/userdata/.cache"
export TMPDIR="$APP/cache" LANG=C.UTF-8 LC_ALL=C.UTF-8
export PATH="$APP/bin:/usr/local/Kobo:/usr/sbin:/usr/bin:/sbin:/bin"
export REEDEN_LOWMEM_SUPPORTED=0 REEDEN_LOWMEM_DEFAULT=0 REEDEN_LOWMEM_FILE="$APP/userdata/lowmem"
export FLUTTER_FONT_DIRECTORY="$APP/fonts/"
# A Kindle self-update must never replace the Kobo host. No auth checks altered.
export REEDEN_UPDATE_FEED=http://127.0.0.1:9/kobo-updates-disabled.json
export GIO_USE_VFS=local GSETTINGS_BACKEND=memory
export SSL_CERT_FILE="$APP/certs/ca-certificates.crt" SSL_CERT_DIR="$APP/certs"
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
cd "$APP/core" || exit 18
"$APP/runtime/reeden-kindle" --library-path "$APP/runtime:$APP/lib:$APP/core/lib" --preload "$APP/lib/libreeden_kobo_compat.so" "$APP/bin/reeden-kindle" >> "$APP/logs/current.log" 2>&1 &
HOST=$!
echo "$HOST $(token "$HOST")" > "$STATE/host"
# Initialize the app while Nickel remains responsive. No framebuffer writes or
# input grabs occur until the engine has delivered an actual frame in memory.
attempt=0
while [ ! -f "$STATE/ready" ]; do
    owned_host || break
    [ -d "$STATE/active" ] && [ ! -d "$STATE/recovering" ] || exit 19
    if [ "$attempt" -ge 20 ]; then echo '[launcher] no first frame in 20s; Nickel was never paused'; exit 20; fi
    sleep 1; attempt=$((attempt+1))
done
if [ -f "$STATE/ready" ]; then
    [ -d "$STATE/active" ] && [ ! -d "$STATE/recovering" ] && [ "$(cat "$STATE/owner" 2>/dev/null)" = "$$ $(token $$)" ] || exit 19
    # KOReader or USB mode may have been entered during warmup. Recheck.
    for pid in $(pidof luajit 2>/dev/null); do
        if tr '\000' ' ' < "/proc/$pid/cmdline" | grep -q reader.lua; then echo 'KOReader entered during warmup; cancel'; exit 10; fi
    done
    pidof nickel >/dev/null 2>&1 || exit 11
    "$APP/native-session.sh" capture || exit 16
    "$APP/bin/fb-guard" --snapshot "$STATE/framebuffer" || exit 15
    "$APP/native-session.sh" stop || exit 17
    [ -d "$STATE/active" ] && [ ! -d "$STATE/recovering" ] && [ "$(cat "$STATE/owner" 2>/dev/null)" = "$$ $(token $$)" ] || exit 19
    : > "$STATE/handoff"
    echo '[launcher] first frame ready; hardware handoff authorized'
fi
wait "$HOST"
code=$?
echo "[launcher] host exit=$code"
exit "$code"
