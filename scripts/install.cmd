@echo off
rem Installs the Speak2Mic driver (x64). Run as Administrator from the package folder.
rem The driver is test-signed: enable test mode first  ->  bcdedit /set testsigning on  (then reboot;
rem in a VMware VM turn off "Enable UEFI Secure Boot" in VM Settings -> Options -> Advanced first).

net session >nul 2>&1 || (echo Запустите этот файл от имени администратора. & pause & exit /b 1)
cd /d "%~dp0"

bcdedit /enum {current} | findstr /i "testsigning" | findstr /i "Yes" >nul
if errorlevel 1 (
    echo ВНИМАНИЕ: тестовый режим подписи не включён, драйвер не загрузится.
    echo Выполните:  bcdedit /set testsigning on   и перезагрузитесь, затем запустите установку снова.
    pause
    exit /b 1
)

rem Trust the test certificate the driver is signed with.
if exist Speak2Mic.cer (
    certutil -addstore -f Root Speak2Mic.cer >nul
    certutil -addstore -f TrustedPublisher Speak2Mic-Publisher.cer >nul
)

echo Если Windows спросит про издателя драйвера, выберите "Все равно установить этот драйвер".
if exist s2minstall.exe (
    s2minstall.exe install Speak2Mic.inf
) else (
    devcon.exe install Speak2Mic.inf ROOT\Speak2Mic
)
if errorlevel 1 if not errorlevel 3010 (
    echo Установка не удалась.
    pause
    exit /b 1
)
echo.
echo Готово: в настройках звука появились "Speak2Mic Speaker" и "Speak2Mic Microphone". Запустите Speak2Mic.exe.
pause
