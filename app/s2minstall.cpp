// s2minstall - console installer for the Speak2Mic driver (replacement for "devcon install").
//
//   s2minstall install [Speak2Mic.inf]   trust the test certificate, create ROOT\Speak2Mic, install the driver
//   s2minstall remove                    remove the device and the driver package
//   s2minstall status                    show whether the device exists
//   s2minstall diag                      full diagnostics (device, service, driver log, sound devices)
// Everything is also written to %ProgramData%\Speak2Mic\logs\install.log.
//
// Needs administrator rights (the manifest requests them). The GUI installer is Speak2Mic-Setup.exe.
#include "setupcore.h"
#include "lang.h"
#include "diag.h"
#include "applog.h"
#include "audiosvc.h"
#include <stdio.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

static void ConsoleLog(void*, const wchar_t* line)
{
    wprintf(L"%ls\n", line);
    AppLog(L"%ls", line);
}

// Our icon and title on the console window (classic console host; Windows Terminal ignores the icon).
static void DecorateConsole()
{
    SetConsoleTitleW(TR(L"Установка Speak2Mic"));
    HWND con = GetConsoleWindow();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (con)
    {
        HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), 0);
        HICON small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), 0);
        if (big) SendMessageW(con, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (small) SendMessageW(con, WM_SETICON, ICON_SMALL, (LPARAM)small);
    }
}

// Path of a file next to this executable.
static void SiblingPath(const wchar_t* name, wchar_t* out)
{
    GetModuleFileNameW(nullptr, out, MAX_PATH);
    wchar_t* slash = wcsrchr(out, L'\\');
    wcscpy(slash ? slash + 1 : out, name);
}

int wmain(int argc, wchar_t** argv)
{
    LangArgs(&argc, argv);
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    DecorateConsole();
    AppLogOpen(L"install");

    if (argc >= 2 && _wcsicmp(argv[1], L"install") == 0)
    {
        AudioServices audio;
        AudioStopServices(false, &audio);
        struct Restart { AudioServices* a; ~Restart() { AudioStartServices(a); } } restart = { &audio };
        if (SetupDeviceCount() > 0 && !SetupDevicePresent())
        {
            ConsoleLog(nullptr, TR(L"Удаление предыдущей версии…"));
            SetupRemoveDriver(ConsoleLog, nullptr, true);    // only a leftover registration; keep the settings
        }
        SetupResetDriverStatus();                           // an existing device is updated in place
        wchar_t cer[MAX_PATH], pub[MAX_PATH];
        SiblingPath(L"Speak2Mic.cer", cer);
        SiblingPath(L"Speak2Mic-Publisher.cer", pub);
        if (GetFileAttributesW(cer) != INVALID_FILE_ATTRIBUTES)
        {
            SetupTrustCertificate(cer, pub, ConsoleLog, nullptr);
        }
        bool reboot = false;
        if (!SetupInstallDriver(argc >= 3 ? argv[2] : L"Speak2Mic.inf", &reboot, ConsoleLog, nullptr))
        {
            return 1;
        }
        SetupRemoveOldDriverPackages(ConsoleLog, nullptr);
        return reboot ? 3010 : 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"remove") == 0)
    {
        AudioServices audio;
        AudioStopServices(false, &audio);
        struct Restart { AudioServices* a; ~Restart() { AudioStartServices(a); } } restart = { &audio };
        bool ok = SetupRemoveDriver(ConsoleLog, nullptr);
        SetupRemoveStaleEndpoints(ConsoleLog, nullptr);
        bool later = false;
        SetupRemoveFiles(&later, ConsoleLog, nullptr);
        if (later) SetupScheduleSelfDelete();
        return ok ? 0 : 1;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"diag") == 0)
    {
        DiagResult r = RunDiagnostics(ConsoleLog, nullptr);
        wprintf(TR(L"\nПодробный журнал: %ls\n"), AppLogPath());
        return r == DiagOk ? 0 : 1;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"status") == 0)
    {
        int n = SetupDeviceCount();
        wprintf(n ? TR(L"Speak2Mic установлен (устройств: %d)\n") : TR(L"Speak2Mic не установлен\n"), n);
        wprintf(TR(L"Тестовый режим подписи: %ls\n"), SetupTestSigningEnabled() ? TR(L"включён") : TR(L"выключен"));
        wprintf(L"Secure Boot: %ls\n", SetupSecureBootEnabled() ? TR(L"включён") : TR(L"выключен"));
        return n ? 0 : 1;
    }

    wprintf(L"s2minstall install [Speak2Mic.inf] | remove | status | diag\n");
    return 1;
}
