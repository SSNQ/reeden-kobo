#!/bin/sh
set -u
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR" || exit 1
echo "Reeden Kobo 保留数据更新"
echo "请先退出 Reeden，连接 USB，并在 Kobo 上点击‘连接’。"
echo "此入口安装本下载包中的版本，不会自动更新电脑上的工具。"
python3 "$DIR/install.py" --update "$@"
status=$?
echo ""
printf "按回车键关闭窗口..."
read -r _
exit "$status"
