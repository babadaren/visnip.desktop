@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0.."
set "QT_BIN=C:\Qt\6.4.2\mingw_64\bin"
set "MINGW_BIN=C:\ProgramData\mingw64\mingw64\bin"
set "PATH=%MINGW_BIN%;%QT_BIN%;C:\ProgramData\chocolatey\bin;%PATH%"

set "SOURCE_EXE=%ROOT%\build\windows-mingw-release\visnip.exe"
set "DIST_DIR=%ROOT%\dist\Visnip"

call "%~dp0configure-release.cmd"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0build-release.cmd"
if errorlevel 1 exit /b %errorlevel%

if not exist "%SOURCE_EXE%" (
  echo Release executable not found: %SOURCE_EXE%
  exit /b 1
)

taskkill /IM visnip.exe /F >nul 2>nul

for /l %%I in (1,1,20) do (
  tasklist /FI "IMAGENAME eq visnip.exe" 2>nul | find /I "visnip.exe" >nul
  if errorlevel 1 goto visnip_stopped
  ping 127.0.0.1 -n 2 >nul
)
echo Timed out waiting for visnip.exe to exit.
exit /b 1

:visnip_stopped
for /l %%I in (1,1,20) do (
  if exist "%DIST_DIR%" rmdir /s /q "%DIST_DIR%" >nul 2>nul
  if not exist "%DIST_DIR%" goto dist_removed
  ping 127.0.0.1 -n 2 >nul
)
echo Package directory is still in use: %DIST_DIR%
exit /b 1

:dist_removed
mkdir "%DIST_DIR%"
if errorlevel 1 exit /b %errorlevel%

copy /y "%SOURCE_EXE%" "%DIST_DIR%\visnip.exe" >nul
if errorlevel 1 exit /b %errorlevel%

"%QT_BIN%\windeployqt.exe" --compiler-runtime --dir "%DIST_DIR%" "%DIST_DIR%\visnip.exe"
if errorlevel 1 exit /b %errorlevel%

for %%F in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
  if exist "%MINGW_BIN%\%%F" (
    if not exist "%DIST_DIR%\%%F" copy /y "%MINGW_BIN%\%%F" "%DIST_DIR%\%%F" >nul
  )
)

rem Local OCR assets (onnxruntime + PP-OCR models), staged next to the build
rem executable by CMake. Without them local OCR mode reports missing components.
if exist "%ROOT%\build\windows-mingw-release\ocr" (
  xcopy /e /i /y "%ROOT%\build\windows-mingw-release\ocr" "%DIST_DIR%\ocr" >nul
  if errorlevel 1 exit /b %errorlevel%
) else (
  echo WARNING: OCR assets missing, local OCR translation will be unavailable.
  echo Run scripts\fetch_ocr_assets.ps1 and package again.
)

echo.
echo Visnip package ready: %DIST_DIR%
echo Run: %DIST_DIR%\visnip.exe
endlocal
