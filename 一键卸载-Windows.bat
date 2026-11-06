@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ==================================================
echo   Reeden Kobo 一键卸载工具 (Windows)
echo ==================================================
where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%~dp0uninstall.py" %*
) else (
    python "%~dp0uninstall.py" %*
)
set "status=%errorlevel%"
echo.
pause
exit /b %status%
