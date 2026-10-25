@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Sync-MavlinkRouter.ps1" %*
set "sync_result=%ERRORLEVEL%"
echo.
if not "%sync_result%"=="0" echo Update stopped. Please read the message above.
pause
exit /b %sync_result%
