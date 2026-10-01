@echo off
setlocal
cd /d "%~dp0.."
python -m tools.ui_region_model.export_diagnostics
if errorlevel 1 exit /b %errorlevel%
python -m tools.ui_region_model.server %*
exit /b %errorlevel%
