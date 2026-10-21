@echo off
rem Cloud90 Game Link: starts the source-built game (see play.ps1).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play.ps1" %*
if errorlevel 1 pause
