@echo off
setlocal
cd /d "%~dp0"
"build\windows\Release\smbd.exe" %*
set "result=%ERRORLEVEL%"
if not "%result%"=="0" if "%~1"=="" pause
exit /b %result%
