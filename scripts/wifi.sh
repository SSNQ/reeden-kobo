#!/bin/sh
# Clara Colour MTK only. Never unload its persistent Wi-Fi modules.
set -u
export PATH=/usr/local/Kobo:/usr/sbin:/usr/bin:/sbin:/bin
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT
mode=${1:-status}
wifi_up() {
    [ -r /sys/class/net/wlan0/flags ] || return 1
    read -r flags < /sys/class/net/wlan0/flags
    case "$flags" in 0x[0-9a-fA-F]*) [ $((flags & 1)) -ne 0 ];; *) return 1;; esac
}
wpa() { timeout 2 wpa_cli -p /var/run/wpa_supplicant -i wlan0 "$@"; }
wifi_connected() {
    wifi_up || return 1
    wpa status 2>/dev/null | grep -qx 'wpa_state=COMPLETED' || return 1
    ifconfig wlan0 2>/dev/null | awk '
      /inet addr:/ { sub("addr:","",$2); if ($2 !~ /^169\.254\./ && $2 !~ /^127\./ && $2 != "0.0.0.0") ok=1 }
      END { exit !ok }'
}
case "$mode" in
enabled) if wifi_up; then echo 1; else echo 0; fi; exit 0;;
status) if wifi_connected; then echo CONNECTED; else echo NA; fi; exit 0;;
signal)
    if ! wifi_connected; then echo '0/5'; exit 0; fi
    rssi=$(wpa signal_poll 2>/dev/null | sed -n 's/^RSSI=\(-\{0,1\}[0-9][0-9]*\)$/\1/p' | head -n 1)
    case "$rssi" in ''|*[!0-9-]*) echo '0/5';;
    *) if [ "$rssi" -ge -55 ]; then n=5; elif [ "$rssi" -ge -65 ]; then n=4; elif [ "$rssi" -ge -75 ]; then n=3; elif [ "$rssi" -ge -85 ]; then n=2; else n=1; fi; echo "$n/5";; esac
    exit 0;;
on|off|resume|suspend|native) ;;
*) exit 64;;
esac
. "$(dirname -- "$0")/session.sh"
product=$(awk -F, '{print substr($NF,length($NF)-2)}' /mnt/onboard/.kobo/version 2>/dev/null)
[ "$product" = 393 ] || exit 69
app_owner() {
    [ -f "$STATE/handoff" ] && [ ! -d "$STATE/recovering" ] &&
    owned_host
}
ui_owner() { [ ! -f "$STATE/asleep" ] && app_owner; }
case "$mode" in on|off) ui_owner || exit 69;; resume|suspend) app_owner || exit 69;; esac
LOCK=/tmp/reeden-kobo-wifi.lock
attempt=0
while ! mkdir "$LOCK" 2>/dev/null; do
    if [ -s "$LOCK/owner" ]; then
        read -r owner start < "$LOCK/owner"
        if ! same_process "$owner" "$start"; then
            rm -f "$LOCK/owner"; rmdir "$LOCK" 2>/dev/null || :
            continue
        fi
    elif [ "$attempt" -ge 20 ]; then
        rmdir "$LOCK" 2>/dev/null || :
    fi
    [ "$attempt" -lt 120 ] || exit 75
    usleep 100000; attempt=$((attempt+1))
done
echo "$$ $(token $$)" > "$LOCK/owner"
release_lock() { rm -f "$LOCK/owner"; rmdir "$LOCK" 2>/dev/null || :; }
trap 'release_lock' EXIT
trap 'exit 143' TERM INT HUP
case "$mode" in on|off) ui_owner || exit 69;; resume|suspend) app_owner || exit 69;; esac
mkdir -p "$APP/logs"
exec >> "$APP/logs/wifi.log" 2>&1
echo "[wifi] request=$mode $(date)"

stop_supplicant() {
    # A process can survive with a missing/broken control socket. Verify exact
    # process identity and interface before signalling those stale instances.
    pids=$(pidof wpa_supplicant 2>/dev/null || :)
    wpa terminate >/dev/null 2>&1 || :
    for pid in $pids; do
        start=$(token "$pid")
        if same_process "$pid" "$start" && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = wpa_supplicant ] &&
           tr '\000' '\n' < "/proc/$pid/cmdline" | grep -Fxq wlan0; then
            kill -TERM "$pid" 2>/dev/null || :
            n=0
            while same_process "$pid" "$start"; do
                [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" != Z ] || break
                [ "$n" -lt 10 ] || { echo '[wifi] stale supplicant did not exit'; return 1; }
                usleep 100000; n=$((n+1))
            done
        fi
    done
}
wifi_off() {
    saved=/tmp/reeden-kobo-wifi-resolv-$$
    [ ! -f /etc/resolv.conf ] || cp /etc/resolv.conf "$saved"
    timeout 4 dhcpcd -d -k wlan0 || :
    if [ -f "$saved" ]; then
        if ! cmp -s "$saved" /etc/resolv.conf; then cat "$saved" > /etc/resolv.conf; fi
        rm -f "$saved"
    fi
    stop_supplicant || return 1
    if [ -r /sys/class/net/wlan0/flags ]; then ifconfig wlan0 down || return 1; fi
    echo 0 > /dev/wmtWifi || return 1
    echo '[wifi] radio off'
}
wifi_on() {
    # Two Kindle properties are set for one UI tap. Do not restart a healthy
    # association on the second request or interrupt an existing connection.
    if wifi_connected; then echo '[wifi] already connected'; return 0; fi
    if ! wifi_up; then
        echo 0xDB9DB9 > /proc/driver/wmt_dbg || return 1
        echo '7 9 0' > /proc/driver/wmt_dbg || return 1
        sleep 1
        echo 0xDB9DB9 > /proc/driver/wmt_dbg || return 1
        echo '7 9 1' > /proc/driver/wmt_dbg || return 1
        echo 1 > /dev/wmtWifi || return 1
        n=0
        while [ ! -r /sys/class/net/wlan0/flags ]; do
            [ "$n" -lt 20 ] || return 1
            usleep 100000; n=$((n+1))
        done
        ifconfig wlan0 up || return 1
    fi
    if ! wpa ping 2>/dev/null | grep -qx PONG; then
        stop_supplicant || return 1
        # Use only the device's existing saved profiles. Never log or edit PSKs.
        timeout 3 wpa_supplicant -D nl80211 -s -i wlan0 -c /etc/wpa_supplicant/wpa_supplicant.conf -C /var/run/wpa_supplicant -B || return 1
        wpa ping 2>/dev/null | grep -qx PONG || return 1
    else
        state=$(wpa status 2>/dev/null | sed -n 's/^wpa_state=//p')
        case "$state" in DISCONNECTED|INACTIVE|INTERFACE_DISABLED)
            wpa reconnect >/dev/null || return 1;; esac
    fi
    # Background DHCP waits for carrier/association itself. Do not hold book
    # wakeup for an absent AP; the UI queries actual association + IPv4 below.
    timeout 3 dhcpcd -d -b -t 15 wlan0 || return 1
    echo '[wifi] association/DHCP started; connection is not yet assumed'
}
case "$mode" in
off) wifi_off && rm -f "$STATE/wifi-restore";;
suspend|native) wifi_off;;
on|resume) wifi_on;;
esac
