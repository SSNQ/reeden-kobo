#!/bin/sh
set -u
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR" || exit 1
echo "=================================================="
echo "  Reeden Kobo 一键卸载工具 (macOS)"
echo "=================================================="
python3 "$DIR/uninstall.py" "$@"
status=$?
echo ""
printf "按回车键关闭窗口..."
read -r _
exit "$status"
