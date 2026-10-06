// Test music of s2mautotest, made at its start in a temporary folder and removed at its end: the same melody (no
// silent passages) as MP3, FLAC (Media Foundation encoders, when Windows has them) and WAV (written directly), one
// folder per format and one with all of them plus a broken file and a text file.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct TestMusicFolder { wchar_t folder[MAX_PATH]; const wchar_t* format; };

struct TestMusic
{
    wchar_t         root[MAX_PATH];
    TestMusicFolder formats[4];          // one file each
    int             formatCount;
    wchar_t         all[MAX_PATH];       // every file + broken.mp3 + readme.txt
    wchar_t         empty[MAX_PATH];     // no music at all (only a text file)
};

typedef void (*MusicLog)(const wchar_t* line);
bool TestMusicCreate(TestMusic* m, MusicLog log);     // false: no file could be made
void TestMusicDelete(TestMusic* m);
