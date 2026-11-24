#!/bin/sh
# Sourced by launcher, guardian and recovery. PID start times avoid PID reuse.
APP=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE="$APP/state"
token_into() {
    _TOKEN=
    [ -r "/proc/$1/stat" ] || return 1
    IFS= read -r _stat_line < "/proc/$1/stat" 2>/dev/null || return 1
    _stat_rest=${_stat_line##*) }
    [ "$_stat_rest" != "$_stat_line" ] || return 1
    case $- in *f*) _noglob=1;; *) _noglob=0;; esac
    set -f
    # shellcheck disable=SC2086
    set -- $_stat_rest
    [ "$_noglob" = 1 ] || set +f
    [ "$#" -ge 20 ] || return 1
    shift 19
    _TOKEN=$1
}
token() { token_into "$1" && printf '%s\n' "$_TOKEN"; }
same_process() { [ -n "$1" ] && [ -n "$2" ] && token_into "$1" && [ "$_TOKEN" = "$2" ]; }
owned_host() {
    [ -f "$STATE/host" ] || return 1
    read -r HOST HOST_TOKEN < "$STATE/host"
    same_process "$HOST" "$HOST_TOKEN" || return 1
    exe=$(readlink "/proc/$HOST/exe")
    case "$exe" in "$APP/bin/reeden-kindle") return 0;; "$APP/runtime/reeden-kindle") tr '\000' '\n' < "/proc/$HOST/cmdline" | grep -Fxq "$APP/bin/reeden-kindle";; *) return 1;; esac
}
