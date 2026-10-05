// Windows audio services around driver restarts/installs (shared by the panel and the installer).
//
// While audiodg.exe has a stream open on a Speak2Mic endpoint (e.g. it is the default device),
// Windows refuses to disable/remove the device cleanly, and the Audio Endpoint Builder can end up
// not rebuilding the endpoints. Stopping the services first avoids that; audio pauses for 1-3 s.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define AUDIO_SVC_MAX 12

struct AudioServices
{
    wchar_t names[AUDIO_SVC_MAX][64];   // services that were stopped, in stop order
    int     count;
};

// Stops "Audiosrv" (and, with includeBuilder, "AudioEndpointBuilder") plus their dependents.
// Needs administrator rights. Details go to the program log.
void AudioStopServices(bool includeBuilder, AudioServices* stopped);

// Starts the services stopped by AudioStopServices again (reverse order).
void AudioStartServices(const AudioServices* stopped);
