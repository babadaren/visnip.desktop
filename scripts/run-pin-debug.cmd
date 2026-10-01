@echo off
setlocal
set "ROOT=%~dp0.."
set "APP=%ROOT%\dist\Visnip\visnip.exe"
set "OUT=%ROOT%\dist\Visnip\visnip-pin-debug.txt"
set "QT_FORCE_STDERR_LOGGING=1"
set "QT_LOGGING_TO_CONSOLE=1"
set "QT_MESSAGE_PATTERN=%%{time yyyy-MM-dd HH:mm:ss.zzz} %%{message}"
echo Writing debug output to: %OUT%
echo Reproduce the issue, then close Visnip from the tray/menu to finish this command.
"%APP%" 1>>"%OUT%" 2>>&1
echo Done. Output: %OUT%
endlocal
