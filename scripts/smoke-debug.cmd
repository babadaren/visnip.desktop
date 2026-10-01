@echo off
setlocal
set PATH=C:\ProgramData\mingw64\mingw64\bin;C:\Qt\6.4.2\mingw_64\bin;%PATH%
build\windows-mingw-debug\visnip.exe --self-test
if errorlevel 1 exit /b %errorlevel%
endlocal
