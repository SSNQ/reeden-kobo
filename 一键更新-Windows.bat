@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ==================================================
echo   Reeden Kobo 保留数据更新
echo   请先退出 Reeden，连接 USB，并在 Kobo 上点击“连接”。
echo   此入口安装本下载包中的版本，不会自动更新电脑上的工具。
echo ==================================================
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%~dp0install.py" --update %*
) else (
    python "%~dp0install.py" --update %*
)
set "status=%errorlevel%"
echo.
pause
exit /b %status%
