#!/bin/sh
# Only the native processes recorded by this session may be stopped.
. "$(dirname -- "$0")/session.sh"
case "${1:-}" in
capture)
    pid=$(pidof nickel) || exit 1
    case "$pid" in *' '*) exit 1;; esac
    tr '\000' '\n' < "/proc/$pid/environ" | grep -E '^(DBUS_SESSION_BUS_ADDRESS|PLATFORM|PRODUCT|WIFI_MODULE|INTERFACE|NICKEL_HOME|LANG|QT_QPA_PLATFORM|QT_PLUGIN_PATH|QT_GSTREAMER_PLAYBIN_AUDIOSINK|QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER)=' > "$STATE/native-env"
    ;;
stop)
    [ -s "$STATE/native-env" ] || exit 1
    : > "$STATE/native-pids"
    for name in sickel hindenburg fickel strickel nickel; do
        for pid in $(pidof "$name" 2>/dev/null); do
            echo "$pid $(token "$pid") $name" >> "$STATE/native-pids"
        done
    done
    : > "$STATE/native-restart"
    while read -r pid start name; do
        if same_process "$pid" "$start"; then kill -TERM "$pid"; kill -CONT "$pid" 2>/dev/null || :; fi
    done < "$STATE/native-pids"
    if command -v usleep >/dev/null 2>&1; then
        term_limit=30; term_wait() { usleep 100000; }
    else
        term_limit=3; term_wait() { sleep 1; }
    fi
    attempt=0
    while [ "$attempt" -lt "$term_limit" ]; do
        remaining=false
        while read -r pid start name; do
            if same_process "$pid" "$start"; then remaining=true; break; fi
        done < "$STATE/native-pids"
        [ "$remaining" = true ] || break
        term_wait; attempt=$((attempt+1))
    done
    while read -r pid start name; do
        if same_process "$pid" "$start" && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$name" ]; then kill -KILL "$pid" 2>/dev/null || :; fi
    done < "$STATE/native-pids"
    # SIGKILL delivery is asynchronous. Do not hand evdev ownership to the new
    # host until every recorded native process has actually released its FDs.
    # A faster event-driven host otherwise exposes this old handoff race.
    attempt=0
    while :; do
        remaining=false
        while read -r pid start name; do
            if same_process "$pid" "$start"; then remaining=true; fi
        done < "$STATE/native-pids"
        [ "$remaining" = true ] || break
        if [ "$attempt" -ge 50 ]; then
            echo '[native] recorded processes did not exit; hardware handoff cancelled' >> "$APP/logs/launcher.log"
            exit 1
        fi
        usleep 100000; attempt=$((attempt+1))
    done
    # Udev opens this FIFO for each hardware notification. With no Nickel reader
    # it would block; this matches the normal KOReader handoff.
    [ ! -p /tmp/nickel-hardware-status ] || rm /tmp/nickel-hardware-status
    ;;
start)
    [ -f "$STATE/native-restart" ] || exit 0
    pidof nickel >/dev/null 2>&1 && exit 0
    # dhcpcd-dbus also owns WPA control sockets and serves ScanResults to
    # Nickel. Across our radio/supplicant restarts it can retain stale sockets
    # and report an empty scan even while wpa_cli sees the APs. Let the system
    # bus activate a fresh bridge for the new Nickel, as on a normal boot.
    # Do not restart the bus itself, or signal unrelated/reused PIDs.
    for pid in $(pidof dhcpcd-dbus 2>/dev/null); do
        start=$(token "$pid")
        if same_process "$pid" "$start" && [ "$(readlink "/proc/$pid/exe")" = /libexec/dhcpcd-dbus ]; then
            kill -TERM "$pid" 2>/dev/null || :
            attempt=0
            while same_process "$pid" "$start"; do
                [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" != Z ] || break
                if [ "$attempt" -ge 20 ]; then
                    echo '[native] network bridge did not exit; leaving it for the system bus' >> "$APP/logs/launcher.log"
                    break
                fi
                usleep 100000; attempt=$((attempt+1))
            done
            echo '[native] network bridge reset requested before Nickel restart' >> "$APP/logs/launcher.log"
        fi
    done
    unset LD_PRELOAD LD_AUDIT LC_ALL XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME
    export HOME=/root TMPDIR=/tmp LD_LIBRARY_PATH=/usr/local/Kobo
    export PATH=/usr/local/Kobo:/usr/sbin:/usr/bin:/sbin:/bin
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK=alsasink
    export QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER=bluealsa:DEV=00:00:00:00:00:00
    while IFS= read -r setting; do
        case "$setting" in DBUS_SESSION_BUS_ADDRESS=*|PLATFORM=*|PRODUCT=*|WIFI_MODULE=*|INTERFACE=*|NICKEL_HOME=*|LANG=*|QT_QPA_PLATFORM=*|QT_PLUGIN_PATH=*|QT_GSTREAMER_PLAYBIN_AUDIOSINK=*|QT_GSTREAMER_PLAYBIN_AUDIOSINK_DEVICE_PARAMETER=*) export "$setting";; esac
    done < "$STATE/native-env"
    if [ -e /tmp/nickel-hardware-status ] && [ ! -p /tmp/nickel-hardware-status ]; then
        mv /tmp/nickel-hardware-status "$STATE/hardware-events-$(date +%s)"
    fi
    [ -p /tmp/nickel-hardware-status ] || mkfifo /tmp/nickel-hardware-status
    cd / || exit 1
    /usr/local/Kobo/hindenburg >/dev/null 2>&1 &
    LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad >> "$APP/logs/native.log" 2>&1 &
    sleep 2
    /sbin/udevadm trigger >/dev/null 2>&1
    pidof nickel >/dev/null 2>&1
    ;;
*) exit 64;;
esac
