@echo off
setlocal
call "%~dp0stop-service.bat"
if errorlevel 1 exit /b 1
call "%~dp0start-service.bat"
exit /b %errorlevel%
