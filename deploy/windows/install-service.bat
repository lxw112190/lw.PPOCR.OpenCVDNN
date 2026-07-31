@echo off
setlocal
cd /d "%~dp0"
set "SERVICE_NAME=lw.PPOCR.OpenCVDNN"
set "DISPLAY_NAME=lw.PPOCR OpenCV DNN HTTP Service"
set "EXE=%CD%\lw-ppocr-http-service.exe"
set "CONFIG=%CD%\http-service.json"

fltmc >nul 2>&1
if errorlevel 1 (
  echo Please run this script as Administrator.
  exit /b 1
)
if not exist "%EXE%" (
  echo HTTP service executable not found: "%EXE%"
  exit /b 1
)
if not exist "%CONFIG%" (
  echo Configuration file not found: "%CONFIG%"
  exit /b 1
)
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if not errorlevel 1 (
  echo Service already exists. Uninstall it before reinstalling.
  exit /b 1
)

sc.exe create "%SERVICE_NAME%" binPath= "\"%EXE%\" --service --config \"%CONFIG%\"" start= auto DisplayName= "%DISPLAY_NAME%"
if errorlevel 1 exit /b 1
sc.exe description "%SERVICE_NAME%" "Cross-platform PP-OCR HTTP service powered by OpenCV DNN."
sc.exe failure "%SERVICE_NAME%" reset= 86400 actions= restart/5000/restart/15000
sc.exe failureflag "%SERVICE_NAME%" 1
sc.exe start "%SERVICE_NAME%"
if errorlevel 1 exit /b 1
echo Service installed and started: %SERVICE_NAME%
exit /b 0
