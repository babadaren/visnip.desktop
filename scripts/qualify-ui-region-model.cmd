@echo off
setlocal
cd /d "%~dp0.."
python -m tools.ui_region_model.qualify %*
exit /b %errorlevel%
