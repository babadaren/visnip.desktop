@echo off
setlocal
set PATH=C:\ProgramData\chocolatey\bin;C:\ProgramData\mingw64\mingw64\bin;C:\Qt\6.4.2\mingw_64\bin;%PATH%
cmake --build --preset release
if errorlevel 1 exit /b %errorlevel%
endlocal
