@echo off
rem Makine Live: cift tiklayin ya da "build.cmd 0.3.1" / "build.cmd --hizli". Ayrinti: tools\build.py
setlocal
cd /d "%~dp0"
if "%~1"=="" (py tools\build.py --sor) else (py tools\build.py %*)
set rc=%errorlevel%
if "%~1"=="" pause
exit /b %rc%
