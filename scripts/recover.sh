#!/bin/sh
. "$(dirname -- "$0")/session.sh"
[ -d "$STATE/active" ] || exit 0
generation=$(cat "$STATE/owner" 2>/dev/null) || generation=''
if [ "${1:-}" != --locked ]; then
    exec "$APP/bin/fb-guard" --recover-session "$APP" "$generation"
fi
[ "$generation" = "${2:-}" ] || exit 0
# Only the kernel-locked worker reaches here; the directory may be from a crash.
mkdir -p "$STATE/recovering" || exit 1
trap 'rmdir "$STATE/recovering" 2>/dev/null || :' EXIT
echo "[recovery] restoring this session" >> "$APP/logs/launcher.log"
if owned_host; then
    kill -TERM "$HOST" 2>/dev/null || :
    i=0
    while owned_host && [ "$i" -lt 5 ]; do sleep 1; i=$((i+1)); done
    if owned_host; then kill -KILL "$HOST" 2>/dev/null || :; sleep 1; fi
fi
case "${HOST:-}" in ''|*[!0-9]*) :;; *) rm -f "/tmp/reeden-kobo-$HOST.heartbeat";; esac
if [ -f "$STATE/night-invert" ]; then
    # Reset our global hardware inversion before Nickel owns the framebuffer.
    if printf 'night_mode 0' > /proc/hwtcon/cmd; then rm -f "$STATE/night-invert"; fi
fi
radio_failed=false
if [ -f "$STATE/native-restart" ]; then
    # Do not reset the radio underneath an already running Nickel.
    if ! pidof nickel >/dev/null 2>&1; then
        touch "$STATE/native-wifi-pending" || echo "[recovery] unable to persist Wi-Fi retry marker" >&2
        if "$APP/power.sh" native; then
            rm -f "$STATE/native-wifi-pending"
        else
            radio_failed=true
            echo "[recovery] native Wi-Fi handoff failed; continuing native UI restoration" >> "$APP/logs/launcher.log"
        fi
    fi
    if ! "$APP/native-session.sh" start; then
        echo "[recovery] native restart failed; session state retained for retry" >> "$APP/logs/launcher.log"
        exit 1
    fi
elif [ -s "$STATE/paused" ]; then
    # Restore framebuffer while Nickel is still paused, then resume exact PIDs.
    if [ -f "$STATE/framebuffer" ]; then
        "$APP/bin/fb-guard" --restore "$STATE/framebuffer" >> "$APP/logs/launcher.log" 2>&1 &
        refresh=$!
        i=0; while kill -0 "$refresh" 2>/dev/null && [ "$i" -lt 5 ]; do sleep 1; i=$((i+1)); done
        if kill -0 "$refresh" 2>/dev/null; then
            kill -KILL "$refresh" 2>/dev/null || :
            echo "[recovery] framebuffer helper timed out; continuing process restoration" >> "$APP/logs/launcher.log"
            # Do not wait on a task stuck in a kernel ioctl: resume Nickel anyway.
        else
            wait "$refresh" 2>/dev/null || :
        fi
    fi
    # Nickel must be responsive before its watchdogs resume checking it.
    for phase in nickel watchdogs; do
        while read -r pid start name; do
            case "$phase:$name" in nickel:nickel|watchdogs:sickel|watchdogs:hindenburg|watchdogs:fickel|watchdogs:strickel) :;; *) continue;; esac
            if same_process "$pid" "$start" && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "$name" ]; then
                kill -CONT "$pid" 2>/dev/null || :
            fi
        done < "$STATE/paused"
        [ "$phase" != nickel ] || sleep 1
    done
fi
if [ -f "$STATE/light" ]; then
    read -r brightness warmth < "$STATE/light"
    case "$brightness" in ''|*[!0-9]*) :;; *) [ "$brightness" -le 100 ] && echo "$brightness" > /sys/class/backlight/mxc_msp430.0/brightness;; esac
    case "$warmth" in ''|*[!0-9]*) :;; *) [ "$warmth" -le 10 ] && echo "$warmth" > /sys/class/backlight/lm3630a_led/color;; esac
fi
rm -f "$STATE/host" "$STATE/owner" "$STATE/paused" "$STATE/light" "$STATE/framebuffer" "$STATE/heartbeat" "$STATE/ready" "$STATE/handoff" "$STATE/asleep" "$STATE/power-cancel" "$STATE/power-phase" "$STATE/power-phase.new" "$STATE/native-restart" "$STATE/native-env" "$STATE/native-pids"
rmdir "$STATE/active" || { echo "[recovery] active state could not be cleared" >> "$APP/logs/launcher.log"; exit 1; }
if [ "$radio_failed" = true ] || [ -f "$STATE/native-wifi-pending" ]; then
    echo "[recovery] native Wi-Fi handoff pending; retry before the next native start" >> "$APP/logs/launcher.log"
    exit 1
fi
echo "[recovery] completed" >> "$APP/logs/launcher.log"
