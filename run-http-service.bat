@echo off
setlocal
cd /d "%~dp0"
"%~dp0lw-ppocr-http-service.exe" --config "%~dp0http-service.json" %*
endlocal
