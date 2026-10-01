@echo off
setlocal
cd /d "%~dp0.."
if "%LOCALAPPDATA%"=="" (
  echo LOCALAPPDATA is unavailable. 1>&2
  exit /b 2
)
python -m tools.ui_region_model.train --dataset "%LOCALAPPDATA%\Visnip\Visnip\ui-region-dataset" %*
exit /b %errorlevel%
