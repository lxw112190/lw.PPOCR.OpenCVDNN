@echo off
setlocal
cd /d "%~dp0"
set "SERVICE_NAME=lw.PPOCR.OpenCVDNN"

fltmc >nul 2>&1
if errorlevel 1 (
  echo Please run this script as Administrator.
  exit /b 1
)
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
  echo Service is not installed: %SERVICE_NAME%
  exit /b 0
)
call "%~dp0stop-service.bat" >nul 2>&1
sc.exe delete "%SERVICE_NAME%"
if errorlevel 1 exit /b 1
echo Service uninstalled: %SERVICE_NAME%
exit /b 0
