@echo off
setlocal
cd /d "%~dp0\.."

python tools\verify_runner.py
exit /b %ERRORLEVEL%
