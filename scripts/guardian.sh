#!/bin/sh
. "$(dirname -- "$0")/session.sh"
read -r OWNER OWNER_TOKEN < "$STATE/owner" || exit 1
GENERATION="$OWNER $OWNER_TOKEN"
started=$(date +%s)
first_update=false
while [ -d "$STATE/active" ]; do
    now=$(date +%s)
    if ! same_process "$OWNER" "$OWNER_TOKEN"; then break; fi
    if [ -f "$STATE/host" ] && ! owned_host; then break; fi
    # Backup deadline if launcher stalls. Most failures now leave Nickel running.
    if [ "$first_update" = false ]; then
        if grep -q FIRST_UPDATE_SUBMITTED "$APP/logs/current.log"; then first_update=true
        elif [ $((now-started)) -ge 30 ]; then
            echo "[guardian] first-update timeout" >> "$APP/logs/launcher.log"; break
        fi
    fi
    heartbeat="/tmp/reeden-kobo-${HOST:-none}.heartbeat"
    # Compatibility with the previous installed host during recovery/update.
    [ ! -f "$STATE/heartbeat" ] || heartbeat="$STATE/heartbeat"
    check_heartbeat=true
    if [ -f "$STATE/asleep" ]; then
        # A suspended kernel stops this guardian too. Once the helper has
        # returned from mem, sleeping is no longer an unlimited exemption.
        phase=none; phase_at=0
        if [ -f "$STATE/power-phase" ]; then read -r phase phase_at phase_pid < "$STATE/power-phase"; fi
        case "$phase:$phase_at" in
            suspended:*) check_heartbeat=false;;
            preparing:*|resuming:*|complete:*)
                check_heartbeat=false
                read -r boot_seconds boot_rest < /proc/uptime
                boot_seconds=${boot_seconds%%.*}
                case "$phase_at" in ''|*[!0-9]*) phase_at=$boot_seconds;; esac
                if [ $((boot_seconds-phase_at)) -gt 45 ]; then
                    echo "[guardian] power phase $phase exceeded 45s; recovering native UI $(date)" >> "$APP/logs/launcher.log"
                    break
                fi
                ;;
        esac
    fi
    # A charging/screen-only lock does not suspend the host or its heartbeat.
    if [ "$check_heartbeat" = true ]; then
        if [ -f "$heartbeat" ]; then
            last=$(stat -c %Y "$heartbeat" 2>/dev/null || echo "$started")
            if [ $((now-last)) -gt 20 ]; then echo "[guardian] heartbeat timeout" >> "$APP/logs/launcher.log"; break; fi
        elif [ $((now-started)) -ge 30 ]; then
            echo "[guardian] no heartbeat" >> "$APP/logs/launcher.log"; break
        fi
    fi
    sleep 5
done
[ -f "$STATE/owner" ] && [ "$(cat "$STATE/owner")" = "$GENERATION" ] || exit 0
exec "$APP/recover.sh"
