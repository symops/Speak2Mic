// Background music for the cable: plays the .mp3 files of a folder in random order on an audio endpoint (shared
// mode, own thread, decoded with Windows Media Foundation). The folder is listed again for every track and on every
// Play. Pause keeps the track and its position; Play continues that track if the file is still there, else starts a
// random one.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Pass as the folder to play the generator instead of files: endless pleasant music synthesized on the fly
// (tracks "::generator\<number>" of 45..90 s, each with its own key, mode and tempo).
#define MP3_GENERATOR L"::generator"

bool Mp3FolderHasFiles(const wchar_t* folder);
// The "mp3" folder of this program: next to the exe (installed: C:\Program Files\Speak2Mic\mp3), or one level up
// (the package: the programs are in x64\ / x86\, the music in the package root).
void Mp3DefaultFolder(wchar_t* folder);
// Starts playing on deviceId. When the playback ends by itself, `msg` is posted to hwnd with wParam = HRESULT
// (S_FALSE: no .mp3 files left in the folder). Every following track (not the first one, which Mp3LastFile() names
// right after this call) posts `trackMsg` (0 = none) with lParam = its file name, malloc'ed: the receiver free()s it.
// Random order; never the same track twice in a row unless it is the only file.
bool Mp3Play(const wchar_t* deviceId, const wchar_t* folder, HWND hwnd, UINT msg, UINT trackMsg = 0);
void Mp3Pause();            // stops (returns when the thread has ended) and remembers the position
bool Mp3Playing();
const wchar_t* Mp3LastFile();   // full path of the track playing / played last ("" = none yet)
const wchar_t* Mp3ResumeFile(); // the track Play continues after a pause ("" = a new one: paused between two tracks)
