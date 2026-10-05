// Program files and shortcuts: copies the package to %ProgramFiles%\Speak2Mic and creates
// (optionally) desktop / Start menu shortcuts for all users. See setupcore.h.
#include "setupcore.h"
#include "lang.h"
#include "applog.h"
#include "autostart.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

// Known folder IDs declared locally (no extra import library needed).
static const GUID kFolderProgramFiles   = { 0x905e63b6, 0xc1bf, 0x494e, { 0xb2, 0x9c, 0x65, 0xb7, 0x32, 0xd3, 0xd2, 0x1a } };
static const GUID kFolderPublicDesktop  = { 0xc4aa340d, 0xf20f, 0x4863, { 0xaf, 0xef, 0xf8, 0x7e, 0xf2, 0xe6, 0xba, 0x25 } };
static const GUID kFolderCommonPrograms = { 0x0139d44e, 0x6afe, 0x49f2, { 0x86, 0x90, 0x3d, 0xaf, 0xca, 0xe6, 0xff, 0xb8 } };
static const GUID kFolderProgramData    = { 0x62ab5d82, 0xfdc1, 0x4dc3, { 0xa9, 0xdd, 0x07, 0x0d, 0x1d, 0x49, 0x5d, 0x97 } };
static const CLSID kClsidShellLink      = { 0x00021401, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID kIidShellLinkW         = { 0x000214f9, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID kIidPersistFile        = { 0x0000010b, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

// Files of the package that are installed to Program Files.
static const wchar_t* kFiles[] = {
    L"Speak2Mic.exe", L"Speak2Mic-Setup.exe", L"s2minstall.exe", L"s2mautotest.exe", L"s2mctl.exe",
    // no longer shipped (removed from the package); listed so that an older installation is cleaned up:
    L"s2mdebug.exe", L"install.cmd", L"УСТАНОВКА.txt", L"INSTALL.txt",
    L"s2mrepeater.exe",     // no longer shipped; listed so that an older installation is cleaned up
    L"Speak2Mic.sys", L"Speak2Mic.inf", L"Speak2Mic.cat", L"Speak2Mic.cer", L"Speak2Mic-Publisher.cer",
    L"uninstall.cmd",
    L"config.ps1",          // no longer shipped (replaced by s2mctl.exe); listed so that an older installation is cleaned up
};

static const wchar_t kLinkPanel[] = L"Speak2Mic.lnk";
// Start menu shortcut names are translated (TR keys); removal tries every language.
#define kLinkSetup L"Speak2Mic — установка и диагностика.lnk"
#define kLinkLogs  L"Журналы Speak2Mic.lnk"

static void Log(SetupLog log, void* ctx, const wchar_t* fmt, ...)
{
    if (!log) return;
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 512, fmt, args);
    va_end(args);
    buf[511] = 0;
    log(ctx, buf);
}

static bool KnownFolder(const GUID& id, wchar_t* out, const wchar_t* sub)
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p))) return false;
    if (sub) _snwprintf(out, MAX_PATH, L"%ls\\%ls", p, sub);
    else _snwprintf(out, MAX_PATH, L"%ls", p);
    out[MAX_PATH - 1] = 0;
    CoTaskMemFree(p);
    return true;
}

bool SetupProgramDir(wchar_t* dir)
{
    return KnownFolder(kFolderProgramFiles, dir, L"Speak2Mic");
}

bool SetupInstallFiles(const wchar_t* sourceDir, SetupLog log, void* ctx)
{
    wchar_t dest[MAX_PATH];
    if (!SetupProgramDir(dest))
    {
        Log(log, ctx, TR(L"Ошибка: не удалось определить папку Program Files."));
        return false;
    }
    if (_wcsicmp(sourceDir, dest) == 0)
    {
        Log(log, ctx, TR(L"Программы уже в %ls."), dest);
        return true;
    }
    SHCreateDirectoryExW(nullptr, dest, nullptr);

    bool ok = true;
    for (const wchar_t* name : kFiles)
    {
        wchar_t from[MAX_PATH], to[MAX_PATH];
        _snwprintf(from, MAX_PATH, L"%ls\\%ls", sourceDir, name);
        _snwprintf(to, MAX_PATH, L"%ls\\%ls", dest, name);
        from[MAX_PATH - 1] = to[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(from) == INVALID_FILE_ATTRIBUTES)
        {
            // Not in this package: a file of an older version (s2mdebug.exe, install.cmd, ...) - remove it.
            if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES && DeleteFileW(to)) AppLog(L"removed obsolete %ls", to);
            continue;
        }
        if (!CopyFileW(from, to, FALSE))
        {
            DWORD err = GetLastError();
            if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED)
                Log(log, ctx, TR(L"Ошибка: %ls занят — закройте Speak2Mic и повторите."), name);
            else
                Log(log, ctx, TR(L"Ошибка копирования %ls (код %lu)."), name, err);
            ok = false;
        }
    }
    // Music for the panel's "Play": every .mp3 of the package's mp3 folder (files the user added are kept).
    {
        wchar_t pattern[MAX_PATH], mp3Dest[MAX_PATH], mp3Src[MAX_PATH];
        // next to the installer, or in the package root (the installer is in x64\ or x86\)
        _snwprintf(mp3Src, MAX_PATH, L"%ls\\mp3", sourceDir);
        mp3Src[MAX_PATH - 1] = 0;
        if (GetFileAttributesW(mp3Src) == INVALID_FILE_ATTRIBUTES)
        {
            wchar_t up[MAX_PATH];
            _snwprintf(up, MAX_PATH, L"%ls\\..\\mp3", sourceDir);
            up[MAX_PATH - 1] = 0;
            GetFullPathNameW(up, MAX_PATH, mp3Src, nullptr);
        }
        _snwprintf(pattern, MAX_PATH, L"%ls\\*.mp3", mp3Src);
        _snwprintf(mp3Dest, MAX_PATH, L"%ls\\mp3", dest);
        pattern[MAX_PATH - 1] = mp3Dest[MAX_PATH - 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            CreateDirectoryW(mp3Dest, nullptr);
            do
            {
                wchar_t from[MAX_PATH], to[MAX_PATH];
                _snwprintf(from, MAX_PATH, L"%ls\\%ls", mp3Src, fd.cFileName);
                _snwprintf(to, MAX_PATH, L"%ls\\%ls", mp3Dest, fd.cFileName);
                from[MAX_PATH - 1] = to[MAX_PATH - 1] = 0;
                if (!CopyFileW(from, to, FALSE))
                    Log(log, ctx, TR(L"Ошибка копирования %ls (код %lu)."), fd.cFileName, GetLastError());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (ok) Log(log, ctx, TR(L"Программы установлены в %ls."), dest);
    return ok;
}

static bool MakeLink(const wchar_t* linkPath, const wchar_t* target, const wchar_t* args, const wchar_t* workDir,
                     const wchar_t* description, const wchar_t* icon)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(kClsidShellLink, nullptr, CLSCTX_INPROC_SERVER, kIidShellLinkW, (void**)&link)))
        return false;
    link->SetPath(target);
    if (args) link->SetArguments(args);
    if (workDir) link->SetWorkingDirectory(workDir);
    if (description) link->SetDescription(description);
    if (icon) link->SetIconLocation(icon, 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(kIidPersistFile, (void**)&file)) && SUCCEEDED(file->Save(linkPath, TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

bool SetupCreateShortcuts(bool desktop, bool startMenu, SetupLog log, void* ctx)
{
    wchar_t dir[MAX_PATH], panel[MAX_PATH], setup[MAX_PATH], path[MAX_PATH], logs[MAX_PATH];
    if (!SetupProgramDir(dir)) return false;
    _snwprintf(panel, MAX_PATH, L"%ls\\Speak2Mic.exe", dir);
    _snwprintf(setup, MAX_PATH, L"%ls\\Speak2Mic-Setup.exe", dir);
    KnownFolder(kFolderProgramData, logs, L"Speak2Mic\\logs");
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    bool ok = true;

    if (desktop)
    {
        if (KnownFolder(kFolderPublicDesktop, path, kLinkPanel) &&
            MakeLink(path, panel, nullptr, dir, TR(L"Speak2Mic — звук динамиков в виртуальный микрофон"), panel))
            Log(log, ctx, TR(L"Создан ярлык на рабочем столе."));
        else
        {
            Log(log, ctx, TR(L"Не удалось создать ярлык на рабочем столе."));
            ok = false;
        }
    }
    if (startMenu)
    {
        wchar_t menu[MAX_PATH];
        if (KnownFolder(kFolderCommonPrograms, menu, L"Speak2Mic"))
        {
            SHCreateDirectoryExW(nullptr, menu, nullptr);
            wchar_t l1[MAX_PATH], l2[MAX_PATH], l3[MAX_PATH];
            _snwprintf(l1, MAX_PATH, L"%ls\\%ls", menu, kLinkPanel);
            _snwprintf(l2, MAX_PATH, L"%ls\\%ls", menu, TR(kLinkSetup));
            _snwprintf(l3, MAX_PATH, L"%ls\\%ls", menu, TR(kLinkLogs));
            bool a = MakeLink(l1, panel, nullptr, dir, TR(L"Панель управления Speak2Mic"), panel);
            bool b = MakeLink(l2, setup, nullptr, dir, TR(L"Установка, переустановка, удаление и диагностика Speak2Mic"), setup);
            SHCreateDirectoryExW(nullptr, logs, nullptr);
            bool c = MakeLink(l3, logs, nullptr, nullptr, TR(L"Папка с журналами Speak2Mic"), nullptr);
            if (a && b && c) Log(log, ctx, TR(L"Созданы ярлыки в меню «Пуск» (папка Speak2Mic)."));
            else
            {
                Log(log, ctx, TR(L"Не все ярлыки в меню «Пуск» удалось создать."));
                ok = false;
            }
        }
    }
    if (com) CoUninitialize();
    return ok;
}

bool SetupRemoveFiles(bool* deleteLater, SetupLog log, void* ctx)
{
    *deleteLater = false;
    wchar_t path[MAX_PATH], dir[MAX_PATH], self[MAX_PATH];

    // Shortcuts and the start with Windows (of the user running the removal).
    S2mSetAutostart(false, L"", false);
    if (KnownFolder(kFolderPublicDesktop, path, kLinkPanel)) DeleteFileW(path);
    wchar_t menu[MAX_PATH];
    if (KnownFolder(kFolderCommonPrograms, menu, L"Speak2Mic"))
    {
        for (int lang = 0; lang < LangCount(); lang++)
        {
            const wchar_t* links[3] = { kLinkPanel, TrLang(kLinkSetup, lang), TrLang(kLinkLogs, lang) };
            for (const wchar_t* l : links)
            {
                _snwprintf(path, MAX_PATH, L"%ls\\%ls", menu, l);
                path[MAX_PATH - 1] = 0;
                DeleteFileW(path);
            }
        }
        RemoveDirectoryW(menu);
    }

    // Program files (the running installer cannot delete itself: the caller does it after exit).
    if (!SetupProgramDir(dir)) return false;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
    {
        Log(log, ctx, TR(L"Ярлыки удалены."));
        return true;
    }
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    bool ok = true;
    for (const wchar_t* name : kFiles)
    {
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
        path[MAX_PATH - 1] = 0;
        if (_wcsicmp(path, self) == 0)
        {
            *deleteLater = true;
            continue;
        }
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path))
        {
            Log(log, ctx, TR(L"Не удалось удалить %ls (код %lu) — закройте Speak2Mic."), name, GetLastError());
            ok = false;
        }
    }
    // The mp3 folder (with any files the user put there).
    {
        wchar_t pattern[MAX_PATH], mp3Dir[MAX_PATH];
        _snwprintf(mp3Dir, MAX_PATH, L"%ls\\mp3", dir);
        _snwprintf(pattern, MAX_PATH, L"%ls\\*", mp3Dir);
        pattern[MAX_PATH - 1] = mp3Dir[MAX_PATH - 1] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                _snwprintf(path, MAX_PATH, L"%ls\\%ls", mp3Dir, fd.cFileName);
                path[MAX_PATH - 1] = 0;
                DeleteFileW(path);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(mp3Dir);
    }
    if (!*deleteLater) RemoveDirectoryW(dir);
    Log(log, ctx, *deleteLater ? TR(L"Ярлыки и программы удалены; папка %ls удалится после закрытия установщика.")
                               : TR(L"Ярлыки и программы удалены из %ls."), dir);
    return ok;
}

void SetupScheduleSelfDelete()
{
    // A hidden cmd waits for this process to exit, then removes the Program Files folder.
    wchar_t dir[MAX_PATH], cmd[MAX_PATH * 2], sys[MAX_PATH];
    if (!SetupProgramDir(dir)) return;
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH * 2, L"\"%ls\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"%ls\"", sys, dir);
    cmd[MAX_PATH * 2 - 1] = 0;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);   // run outside the folder being removed
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, temp, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
