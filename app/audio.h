// Shared WASAPI helpers for the Speak2Mic programs.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#define S2M_MAX_METER_CHANNELS 8

struct AudioDevice
{
    wchar_t   id[256];
    wchar_t   name[256];    // friendly name as shown by Windows: "<desc> (<adapter>)"
    wchar_t   desc[256];    // the endpoint's own name, e.g. "Speak2Mic Speaker" (user-renamable)
    wchar_t   adapter[64];  // the device it belongs to, "Speak2Mic" for our endpoints
    EDataFlow flow;         // eRender (playback) or eCapture (recording)
};

// COM must be initialized on the calling thread for all functions below.

// Active playback and recording devices; returns the number written to `out`.
int ListAudioDevices(AudioDevice* out, int max);

// Default playback device.
bool GetDefaultRenderDevice(AudioDevice* out);

// ---------------------------------------------------------------------------
// Default ("shared mode") format of an endpoint, as in Sound control panel -> Advanced -> Default Format.
// Uses the same system interface the Sound control panel uses (IPolicyConfig).

// On failure `err` explains why; on success it names the method that worked (for the log).
bool SetEndpointFormat(const wchar_t* deviceId, DWORD rate, WORD bits, WORD channels, wchar_t* err, size_t errLen);
bool GetEndpointFormat(const wchar_t* deviceId, DWORD* rate, WORD* bits, WORD* channels);

// Renames an endpoint (its PKEY_Device_DeviceDesc, like "Rename" in Sound settings). Windows shows
// "<name> (Speak2Mic)" afterwards and keeps the name across driver restarts.
HRESULT SetEndpointName(const wchar_t* deviceId, const wchar_t* name);

// Sound test like "Test" in Windows' speaker properties: Windows' own test melodies (mmres.dll WAVE 3110 on
// odd channels, 3111 on even ones; fallbacks: Media\Windows Background.wav, a built-in chime) played on
// every channel of the playback device in turn.
// Blocking; returns when done, when *stop becomes non-zero, or on an error (HRESULT, S_OK = done/stopped).
HRESULT PlayChannelTest(const wchar_t* deviceId, volatile LONG* stop);

// Endpoint volume 0..1 (the slider in Sound settings / the "Levels" tab). Careful: for a microphone
// without a hardware volume Windows' range is -96..+30 dB, so 1.0 (100 %) there means +30 dB of gain.
bool GetEndpointVolume(const wchar_t* deviceId, float* scalar);
bool SetEndpointVolume(const wchar_t* deviceId, float scalar);
// Endpoint volume in dB (0 dB = the signal passes unchanged); Set clamps to the endpoint's range.
// hrOut: S_OK, or the failure. After a device restart Windows sometimes fails the master level or the range
// (E_INVALIDARG): the loudest channel / the last range read is used then (true) and hrOut keeps that failure code.
bool GetEndpointVolumeDb(const wchar_t* deviceId, float* db, float* minDb, float* maxDb, HRESULT* hrOut = nullptr);
// One line with the result of every IAudioEndpointVolume read (diagnostics).
void DescribeEndpointVolume(const wchar_t* deviceId, wchar_t* out, size_t len);
// Event context of every volume / mute change made by the Speak2Mic programs: an IAudioEndpointVolumeCallback can
// tell them from changes made by Windows or other programs.
extern const GUID kS2mVolumeContext;
// Reports (on a system thread) every volume / mute change of the endpoint that did NOT come from a Speak2Mic program.
// Returns a handle for UnwatchEndpointVolume, or nullptr. The watch ends when the endpoint is recreated.
typedef void (*ForeignVolumeFn)(float db, float scalar, bool mute, const GUID& context, void* ctx);
void* WatchEndpointVolume(const wchar_t* deviceId, ForeignVolumeFn fn, void* ctx);
void  UnwatchEndpointVolume(void* watch);
// The audio sessions open on an endpoint right now: "name.exe (pid N, active|inactive)", "; "-separated. Programs that
// adjust a microphone's volume (WebRTC/Teams/Zoom automatic gain, ...) normally keep a session open on it.
void DescribeAudioSessions(const wchar_t* deviceId, wchar_t* out, size_t len);
// The programs (exe file names, ", "-separated) with an ACTIVE audio session on the endpoint, other than this process:
// the likely authors of a volume change (automatic gain of call / monitoring programs). "" if there is none.
void ActiveSessionPrograms(const wchar_t* deviceId, wchar_t* out, size_t len);
// For a test PLAYBACK stream (autotest): opts its session out of Windows' communications ducking ("reduce / mute other
// sounds while a call app is active") and puts a turned-down or muted session (Volume mixer) back to 100 %. Writes what
// it had to change into `info` ("" = nothing). Not for capture streams: there the session volume is the endpoint's own
// volume (setting it to 100 % sets the microphone to its maximum).
void PrepareTestSession(IAudioClient* client, wchar_t* info, size_t len);
bool SetEndpointVolumeDb(const wchar_t* deviceId, float db);
bool GetEndpointMute(const wchar_t* deviceId, bool* mute);
bool SetEndpointMute(const wchar_t* deviceId, bool mute);

// After a device restart Windows re-applies each endpoint's stored volume, mute and default format a moment later
// (seen up to ~1.5 s), sometimes older values than the last change (a change made a second before the restart is
// not stored yet). HoldEndpointSettings checks every 250 ms for `ms` and sets back whatever differs; returns the
// number of corrections (each one is logged).
struct EndpointHold
{
    const wchar_t* id;
    bool  format;               // hold the default format (rate, bits, channels)
    DWORD rate;
    WORD  bits, channels;
    bool  level;                // hold volume and mute (the values read just before the restart)
    float db;
    bool  mute;
};
int HoldEndpointSettings(EndpointHold* holds, int n, DWORD ms);

struct EndpointVolumeInfo
{
    float scalar, db, minDb, maxDb;
    bool  mute;
    bool  hardware;     // the driver implements the volume (otherwise Windows applies it in software)
};
bool GetEndpointVolumeInfo(const wchar_t* deviceId, EndpointVolumeInfo* info);

// Lower-level helpers. `valid` = valid bits (extensible only).
void MakeWaveFormat(WAVEFORMATEXTENSIBLE* f, DWORD rate, WORD container, WORD valid, WORD channels, bool isFloat,
                    bool extensible);
void DescribeWaveFormat(const WAVEFORMATEX* f, wchar_t* out, size_t len);
HRESULT PolicySetFormat(const wchar_t* deviceId, const WAVEFORMATEX* device, const WAVEFORMATEX* mix);
HRESULT PolicyGetFormat(const wchar_t* deviceId, bool defaultFormat, WAVEFORMATEX** format);   // CoTaskMemFree
HRESULT PolicyGetMixFormat(const wchar_t* deviceId, WAVEFORMATEX** format);                     // CoTaskMemFree

// ---------------------------------------------------------------------------
// Level meter: captures a device (loopback for playback devices) on a background thread
// and records the peak level per channel.

class LevelMeter
{
public:
    LevelMeter();
    ~LevelMeter();

    bool Start(const AudioDevice& dev);
    void Stop();
    bool Running() const { return m_thread != nullptr; }
    // The capture stopped by itself (device invalidated, format changed, no access): Start again to reconnect.
    bool Ended() const { return m_ended != 0; }

    // Peak per channel (0..1) since the previous call; returns the channel count (0 if no data yet).
    int TakePeaks(float* peaks, int max);

    // Human-readable stream format, or an error description.
    void GetStatus(wchar_t* buf, size_t len);

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void Loop();
    void SetStatus(const wchar_t* text);

    AudioDevice      m_dev;
    HANDLE           m_thread;
    volatile LONG    m_stop;
    volatile LONG    m_ended;
    CRITICAL_SECTION m_cs;
    float            m_peaks[S2M_MAX_METER_CHANNELS];
    int              m_channels;
    wchar_t          m_status[160];
};
