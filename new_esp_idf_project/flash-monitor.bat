@echo off
setlocal

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM5"

cd /d "%~dp0"
call "%~dp0idf-env.bat"

idf.py -p %PORT% flash monitor
