@echo off
rem Makine Live: sunucusuz test (yerel sahte sunucu). Ayrinti: tools\offline.py
setlocal
cd /d "%~dp0"
py tools\offline.py %*
set rc=%errorlevel%
if "%~1"=="" pause
exit /b %rc%
