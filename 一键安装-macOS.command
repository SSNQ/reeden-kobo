#!/bin/sh
set -u
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR" || exit 1
echo "=================================================="
echo "  Reeden Kobo 一键组包与安装工具 (macOS)"
echo "=================================================="
python3 "$DIR/install.py" "$@"
status=$?
echo ""
printf "按回车键关闭窗口..."
read -r _
exit "$status"
