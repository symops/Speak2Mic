@echo off
rem Removes the Speak2Mic device and its driver package. Run as Administrator.
net session >nul 2>&1 || (echo Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"

rem The package root has x64\ and x86\ (64-bit / 32-bit Windows); an installed copy has s2minstall.exe next to it.
set ARCH=x86
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set ARCH=x64
if /i "%PROCESSOR_ARCHITEW6432%"=="AMD64" set ARCH=x64
if exist s2minstall.exe (s2minstall.exe remove) else if exist %ARCH%\s2minstall.exe (%ARCH%\s2minstall.exe remove) else (devcon.exe remove ROOT\Speak2Mic)

rem Delete the driver package(s) from the driver store (they are published as oemNN.inf).
powershell -NoProfile -Command ^
  "$d = pnputil /enum-drivers | Out-String; " ^
  "$blocks = $d -split '(\r?\n){2,}'; " ^
  "foreach ($b in $blocks) { if ($b -match 'speak2mic.inf' -and $b -match '(oem\d+\.inf)') { pnputil /delete-driver $Matches[1] /uninstall /force } }"
echo Готово.
pause
