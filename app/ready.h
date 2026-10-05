// The "cannot work" notice for S2mCheckReady() in the interface language (programs that use lang.h).
#pragma once

#include "devctl.h"
#include "lang.h"

inline const wchar_t* S2mNotReadyText(S2mNotReady reason)
{
    switch (reason)
    {
    case S2mNotReadySecureBoot:
        return TR(L"Speak2Mic не может работать: включён Secure Boot, а драйвер Speak2Mic с тестовой подписью при нём не загружается.\n\nВыключите Secure Boot в настройках UEFI (VMware: VM → Settings → Options → Advanced), затем запустите Speak2Mic-Setup.exe.");
    case S2mNotReadyTestMode:
        return TR(L"Speak2Mic не может работать: тестовый режим подписи Windows выключен, без него драйвер Speak2Mic не загружается.\n\nЗапустите Speak2Mic-Setup.exe, нажмите «Включить тестовый режим» и перезагрузите компьютер.");
    case S2mNotReadyDriver:
        return TR(L"Speak2Mic не может работать: драйвер Speak2Mic не установлен.\n\nЗапустите Speak2Mic-Setup.exe и нажмите «Установить».");
    default:
        return L"";
    }
}
