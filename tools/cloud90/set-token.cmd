@echo off
rem Cloud90 Game Link: saves the game token (see set-token.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-token.ps1" %*
pause
