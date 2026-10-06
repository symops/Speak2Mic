// Test music of s2mautotest, made at its start in a temporary folder and removed at its end: the same melody (no
// silent passages) as MP3, FLAC (Media Foundation encoders, when Windows has them) and WAV (written directly), one
// folder per format and one with all of them plus a broken file and a text file.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct TestMusicFolder { wchar_t folder[MAX_PATH]; wchar_t format[64]; };

struct TestMusic
{
    wchar_t         root[MAX_PATH];
    TestMusicFolder formats[24];         // one file each (generated, then the built-in samples)
    int             formatCount;
    wchar_t         all[MAX_PATH];       // every file + broken.mp3 + readme.txt
    wchar_t         empty[MAX_PATH];     // no music at all (only a text file)
};

typedef void (*MusicLog)(const wchar_t* line);
// false: no file could be made. Also writes out the samples built into the program (OGG Vorbis and MP3 / WAV / FLAC
// variants Windows does not write: tools/make_autotest_media.py in Show2Cam), one folder each.
bool TestMusicCreate(TestMusic* m, MusicLog log);
void TestMusicDelete(TestMusic* m);
