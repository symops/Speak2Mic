// Interface language of the Speak2Mic programs.
//
// The Russian text in the source code is the key: TR(L"...") returns its translation in the current
// language (falling back to English, then Russian). Translations live in app/lang/*.txt and are compiled
// into lang_table.inc by lang/gen_lang.py. The language is the user's choice (HKCU\Software\Speak2Mic,
// value "Language", shared by all Speak2Mic programs); without a choice the system's language (Windows
// display language, then locale) when it is one of ours, English otherwise.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct S2mLangInfo
{
    const wchar_t* code;    // "ru", "en", ...
    const wchar_t* name;    // native name for the language list
    WORD           primary; // Windows primary language id (LANG_*)
};

const wchar_t* Tr(const wchar_t* ru);
#define TR(s) Tr(s)
const wchar_t* TrLang(const wchar_t* ru, int lang);   // in a given language (e.g. to find old shortcut names)

int LangCount();
const S2mLangInfo* LangInfo(int index);
int LangCurrent();                       // index of the language in use
int LangSetting();                       // saved choice, -1 = automatic
bool LangSave(int index);                // -1 = automatic; takes effect on the next start
void LangForce(const wchar_t* code);     // for this process only (e.g. "--lang en" on a command line)

// Console programs: removes "--lang <code>" from the arguments and uses that language.
void LangArgs(int* argc, wchar_t** argv);

// Starts this program again with the same command line plus "--restarted" (the caller then closes).
void LangRestartProgram();

// Language selector: fills a combo box with the native language names and selects the current one.
void LangFillCombo(HWND combo);
// Saves the language picked in such a combo box and starts this program again with the same command
// line (the caller then closes its window). Returns false if nothing changed.
bool LangApplyCombo(HWND combo);
