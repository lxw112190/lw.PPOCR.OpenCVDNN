@echo off
setlocal
set "SERVICE_NAME=lw.PPOCR.OpenCVDNN"
fltmc >nul 2>&1
if errorlevel 1 (
  echo Please run this script as Administrator.
  exit /b 1
)
sc.exe start "%SERVICE_NAME%"
exit /b %errorlevel%
