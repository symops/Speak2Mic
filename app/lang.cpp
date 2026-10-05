// Interface language (see lang.h).
#include "lang.h"
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>

#include "lang_table.inc"

#define S2M_LANG_KEY L"Software\\Speak2Mic"

static int g_lang = -2;         // -2 = not decided yet

static int FindCode(const wchar_t* code)
{
    for (int i = 0; i < S2M_LANG_COUNT; i++)
        if (_wcsicmp(kLangs[i].code, code) == 0) return i;
    return -1;
}

int LangSetting()
{
    wchar_t code[16] = L"";
    DWORD size = sizeof(code);
    if (RegGetValueW(HKEY_CURRENT_USER, S2M_LANG_KEY, L"Language", RRF_RT_REG_SZ, nullptr, code, &size) != ERROR_SUCCESS)
        return -1;
    return FindCode(code);
}

static int FindPrimary(LANGID id)
{
    WORD primary = PRIMARYLANGID(id);
    for (int i = 0; i < S2M_LANG_COUNT; i++)
        if (kLangs[i].primary == primary) return i;
    return -1;
}

// The user's explicit choice; otherwise the system's language: Windows display language, then the user's
// regional format (locale), then the system locale; English when none of them is one of ours.
static int Detect()
{
    int saved = LangSetting();
    if (saved >= 0) return saved;
    int i = FindPrimary(GetUserDefaultUILanguage());
    if (i < 0) i = FindPrimary(LANGIDFROMLCID(GetUserDefaultLCID()));
    if (i < 0) i = FindPrimary(LANGIDFROMLCID(GetSystemDefaultLCID()));
    return i >= 0 ? i : 1;
}

int LangCurrent()
{
    if (g_lang == -2) g_lang = Detect();
    return g_lang;
}

void LangForce(const wchar_t* code)
{
    int i = FindCode(code);
    if (i >= 0) g_lang = i;
}

bool LangSave(int index)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, S2M_LANG_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const wchar_t* code = index >= 0 && index < S2M_LANG_COUNT ? kLangs[index].code : L"auto";
    bool ok = RegSetValueExW(key, L"Language", 0, REG_SZ, (const BYTE*)code,
                             (DWORD)((wcslen(code) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

int LangCount() { return S2M_LANG_COUNT; }

void LangArgs(int* argc, wchar_t** argv)
{
    for (int i = 1; i + 1 < *argc; i++)
    {
        if (_wcsicmp(argv[i], L"--lang") == 0)
        {
            LangForce(argv[i + 1]);
            for (int k = i; k + 2 <= *argc; k++) argv[k] = argv[k + 2];
            *argc -= 2;
            return;
        }
    }
}

void LangFillCombo(HWND combo)
{
    // Alphabetical by native name, the same order on every system (invariant locale): Latin, then Cyrillic,
    // then the Asian scripts. Item data = language index.
    int order[S2M_LANG_COUNT];
    for (int i = 0; i < S2M_LANG_COUNT; i++) order[i] = i;
    for (int i = 1; i < S2M_LANG_COUNT; i++)
        for (int j = i; j > 0 && CompareStringEx(LOCALE_NAME_INVARIANT, LINGUISTIC_IGNORECASE, kLangs[order[j]].name, -1,
                                                 kLangs[order[j - 1]].name, -1, nullptr, nullptr, 0) == CSTR_LESS_THAN; j--)
        {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int k = 0; k < S2M_LANG_COUNT; k++)
    {
        int item = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)kLangs[order[k]].name);
        SendMessageW(combo, CB_SETITEMDATA, item, order[k]);
        if (order[k] == LangCurrent()) SendMessageW(combo, CB_SETCURSEL, item, 0);
    }
}

bool LangApplyCombo(HWND combo)
{
    int item = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (item < 0) return false;
    int sel = (int)SendMessageW(combo, CB_GETITEMDATA, item, 0);
    if (sel < 0 || sel >= S2M_LANG_COUNT || sel == LangCurrent()) return false;
    LangSave(sel);
    LangRestartProgram();
    return true;
}

void LangRestartProgram()
{
    // Same program, same arguments; an elevated process starts an elevated copy.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // "--restarted": single-instance programs wait for this copy to exit instead of refusing to start.
    size_t len = wcslen(GetCommandLineW()) + 16;
    wchar_t* cmd = (wchar_t*)malloc(len * sizeof(wchar_t));
    if (cmd) _snwprintf(cmd, len, L"%ls --restarted", GetCommandLineW());
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (cmd && CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    free(cmd);
}

const S2mLangInfo* LangInfo(int index)
{
    return index >= 0 && index < S2M_LANG_COUNT ? &kLangs[index] : nullptr;
}

const wchar_t* TrLang(const wchar_t* ru, int lang)
{
    if (!ru || lang <= 0 || lang >= S2M_LANG_COUNT) return ru;
    // A few hundred short strings: a linear search is fast enough (texts change a few times a second at most).
    for (const auto& row : kText)
    {
        if (row[0] == ru || wcscmp(row[0], ru) == 0)
        {
            if (row[lang]) return row[lang];
            return row[1] ? row[1] : ru;
        }
    }
    return ru;
}

const wchar_t* Tr(const wchar_t* ru)
{
    return TrLang(ru, LangCurrent());
}
