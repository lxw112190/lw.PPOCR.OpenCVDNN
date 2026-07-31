@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "SERVICE_NAME=lw.PPOCR.OpenCVDNN"
fltmc >nul 2>&1
if errorlevel 1 (
  echo Please run this script as Administrator.
  exit /b 1
)
sc.exe query "%SERVICE_NAME%" >nul 2>&1
if errorlevel 1 (
  echo Service is not installed: %SERVICE_NAME%
  exit /b 1
)
for /f "tokens=4" %%S in ('sc.exe query "%SERVICE_NAME%" ^| findstr /C:"STATE"') do set "STATE=%%S"
if /I "!STATE!"=="STOPPED" (
  echo Service is already stopped: %SERVICE_NAME%
  exit /b 0
)
sc.exe stop "%SERVICE_NAME%" >nul
if errorlevel 1 exit /b 1
for /L %%I in (1,1,30) do (
  set "STATE="
  for /f "tokens=4" %%S in ('sc.exe query "%SERVICE_NAME%" ^| findstr /C:"STATE"') do set "STATE=%%S"
  if /I "!STATE!"=="STOPPED" (
    echo Service stopped: %SERVICE_NAME%
    exit /b 0
  )
  timeout /t 1 /nobreak >nul
)
echo Timed out waiting for the service to stop.
exit /b 1
