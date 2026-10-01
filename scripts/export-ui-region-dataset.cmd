@echo off
setlocal
cd /d "%~dp0.."
python -m tools.ui_region_model.export_diagnostics %*
exit /b %errorlevel%
