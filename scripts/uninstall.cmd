@echo off
rem Removes the Speak2Mic device and its driver package. Run as Administrator.
net session >nul 2>&1 || (echo Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"

rem The installer removes without a window ("/remove"): the package root has the launcher, x64\ / x86\ and an
rem installed copy have the installer itself. ("start /wait": cmd does not wait for a program with a window.)
start "" /wait "Speak2Mic-Setup.exe" /remove
if errorlevel 2 (echo Перезагрузите компьютер, чтобы закончить удаление.) else if errorlevel 1 (echo Удаление не удалось - см. %ProgramData%\Speak2Mic\logs\setup.log)

rem Delete the driver package(s) from the driver store (they are published as oemNN.inf).
powershell -NoProfile -Command ^
  "$d = pnputil /enum-drivers | Out-String; " ^
  "$blocks = $d -split '(\r?\n){2,}'; " ^
  "foreach ($b in $blocks) { if ($b -match 'speak2mic.inf' -and $b -match '(oem\d+\.inf)') { pnputil /delete-driver $Matches[1] /uninstall /force } }"
echo Готово.
pause
