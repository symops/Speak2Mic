# Speak2Mic — virtual audio cable for Windows 10/11

Speak2Mic works like Virtual Audio Cable: its driver adds a pair of audio devices connected by a "cable".

| Device | Type | What it does |
|---|---|---|
| **Speak2Mic Speaker** | playback (line out) | everything played into it… |
| **Speak2Mic Microphone** | recording (**microphone**) | …is heard on this microphone |

There is one cable. The programs speak 17 languages (the Windows language by default, otherwise English).

> **Important.** The driver is signed with a test certificate, so Windows must run in test signing mode (Secure Boot off). Install it in a virtual machine with a snapshot: a bug in a kernel driver means a blue screen.

## The control panel (`Speak2Mic.exe`)

- **Signal quality:** presets (Voice / Standard / High / Studio / Maximum) or sample rate, bit depth, channels and latency by hand. "Apply" restarts the driver with the new settings.
- **Level meters:** live per-channel meters (dBFS, peak hold) for both ends of the cable, plus the format Windows actually uses.
- **Microphone channels:** 1 (mono) by default in every preset; 2, 4, 6, 8 or "Same as the speaker" can be chosen. With fewer microphone channels than speaker channels the cable downmixes (mono = average of left and right); with more, a mono source is copied to every channel and the rest stay silent. The default formats are set in the INF: speaker 48 kHz, 16 bit, stereo; microphone 48 kHz, 16 bit, mono.
- **Microphone volume:** a slider of 0–300 % signal level (100 % = 0 dB, the cable's signal unchanged; 200 % ≈ +6 dB; 300 % ≈ +9.5 dB) that snaps to 0, 100, 200 and 300 %, and a "Mute" box. This is the Windows volume of the microphone, applied by the driver's own volume node (−96…+9.56 dB), so Windows' "100 %" in the Sound control panel equals 300 % here. While the panel is open it puts the volume back when another program changes it and names that program in the event list.
- **Test** plays Windows' test melody on every speaker channel in turn; **Play / Pause** plays music files (mp3, wav, flac, ogg) from the `mp3` folder next to the program in random order into the cable (pause remembers track and position; the button is disabled without mp3 files).
- **Device names:** "Speak2Mic Speaker" and "Speak2Mic Microphone" can be renamed (an empty name means the default). The panel remembers them and restores them after a reinstallation.
- **Export / import** of all settings except the language (`.ini`, UTF-16) and **Reset all settings** (preset "Standard", default names, 100 %, sound on).
- **Event list** at the bottom with date and time, kept across sessions, with a button to clear it.
- **Tray and autostart:** minimizing hides the panel in the notification area (volume protection and music keep running). The installer starts the panel with Windows, in the tray (`Speak2Mic.exe /t`); a box at the bottom of the panel turns this on and off.

## Use case: sound inside a VMware VM → virtual microphone

Inside the Windows guest:

1. Settings → System → Sound: choose the output device **"Speak2Mic Speaker"**. All sound now goes into the microphone "Speak2Mic Microphone".
2. To hear it as well: Control Panel → Sound → Recording → "Speak2Mic Microphone" → Properties → Listen → "Listen to this device", and pick the VMware speakers.

A single program can be routed into the cable instead: Settings → Sound → Volume mixer → output "Speak2Mic Speaker" for that app.

If a call program uses Speak2Mic Microphone, set Control Panel → Sound → Communications to "Do nothing": otherwise Windows turns down "other sounds", including what you play into the cable.

## Signal quality

| Preset | Sample rate | Bits | Channels |
|---|---|---|---|
| Voice | 16 kHz | 16 | mono |
| Standard | 48 kHz | 16 | stereo |
| High | 48 kHz | 24 | stereo |
| Studio | 96 kHz | 24 | stereo |
| Maximum | 192 kHz | 32 | stereo |

"Apply" (with the UAC shield) asks for administrator rights, writes the settings and restarts the device; streams open on the cable are interrupted for 2–3 seconds. Afterwards the panel sets the chosen format as the Windows "Default Format" of both devices (through `IPolicyConfig`, like the Sound control panel; Windows would keep the old one otherwise). The driver always accepts 16, 24 and 32 bit; if Windows refuses a 24/32-bit integer default format, the panel uses 16 bit at the same rate and says so. "Auto" means 24 bit. Updates and reinstallations keep the settings; "Remove" resets them to "Standard".

Without the interface: `s2mctl.exe` (administrator command prompt):
```
s2mctl status
s2mctl set --rate 48000 --bits 24 --latency 40
```

| Setting | Default | Meaning |
|---|---|---|
| `SampleRate` | 48000 | the cable's sample rate (the only one it supports) |
| `Channels` | 2 | speaker channels, 1–8 |
| `MicChannels` | 1 | microphone channels, 1–8; 0 = same as the speaker |
| `BitsPerSample` | 16 | 16, 24, 32, or 0 = any of them |
| `LatencyMs` | 30 | how far the microphone lags behind the speaker; raise it if you hear clicks |

They live in `HKLM\SYSTEM\CurrentControlSet\Services\Speak2Mic\Parameters` and are read when the device starts.

## Installation (VMware VM)

1. Shut the VM down. VM → Settings → Options → Advanced: **clear "Enable UEFI Secure Boot"** (test signing does not work with it).
2. Copy **`Speak2Mic-Setup.exe`** into the VM and run it. It picks the 64- or 32-bit installer, which asks for administrator rights and shows whether Secure Boot is off, whether test signing mode is on and whether the driver is installed.
3. If test signing mode is off, click "Enable test mode", restart and run the installer again. The same button turns the mode off later; without it the driver does not load and the Speak2Mic devices disappear until the mode is back. "Install" stays disabled until Secure Boot is off and test mode is active.
4. Optionally tick the desktop and Start menu shortcuts and click "Install". If Windows asks about the publisher, choose "Install this driver software anyway". The programs go to `C:\Program Files\Speak2Mic\`.
5. "Speak2Mic Speaker" and "Speak2Mic Microphone" appear in the Sound settings and the installer opens the panel.

Removal: `Speak2Mic-Setup.exe` → "Remove" (device, driver package, programs, shortcuts, autostart), or `uninstall.cmd` as administrator.

The panel, `s2mctl` and the autotest check at start that Secure Boot is off, test signing mode is on and the driver is installed, and refuse to work otherwise (the installers always run: they are what fixes it).

## Building (Linux)

```
./package.sh
```
needs clang, MinGW-w64 for x86_64 and i686 (gcc, windres, dlltool), a host C compiler and make (for `tools/generate-cat-file`), osslsigncode, openssl, gcab and Python 3 (on Debian/Ubuntu: `clang mingw-w64 gcc make osslsigncode openssl gcab python3`). It builds the driver and the programs for x64 and x86 and assembles:

- **`dist/Speak2Mic-Setup.exe`** — the whole package in one file (~29 MB with music): it unpacks itself to a temporary folder, starts the installer for the right bitness and cleans up afterwards;
- **`dist/Speak2Mic/`** — the same unpacked: a launcher `Speak2Mic-Setup.exe`, full `x64\` and `x86\` sets (installer, panel, `s2minstall.exe`, `s2mctl.exe`, `s2mautotest.exe`, test-signed `Speak2Mic.sys`, `Speak2Mic.inf`, `Speak2Mic.cat`, certificates), `mp3\` and `uninstall.cmd`. Windows on ARM is not supported.

Not in this repository (see `.gitignore`):

- **Test signing keys** (`driver/mingw/testcert/`). `driver/mingw/build.sh` creates a new test root CA and signing certificate when they are missing. Keep them private: the installer trusts that root on every machine it installs on.
- **`infverif.exe`** for `check_inf.sh`: take it from the WDK NuGet package `microsoft.windows.wdk.x64` (`c/tools/<version>/x64/infverif.exe`) and put it into `tools/infverif/`; it runs under Wine.

How the driver is built without the WDK (`driver/mingw/`): clang compiles it against the MinGW DDK headers with `-mno-red-zone`; import libraries for `portcls.sys`/`ntoskrnl.exe` are generated from `.def` files and `CUnknown` (`stdunk.lib` in the WDK) is implemented in `stdunk_impl.cpp`; the image is linked as a native driver, `pefix.py` makes its PE header look like a WDK one and `osslsigncode` test-signs it (SHA-256). The catalog `Speak2Mic.cat` (signed hashes of the INF and SYS; without it Windows refuses the package with `0xE000022F`) is made by [LINBIT generate-cat-file](https://github.com/LINBIT/generate-cat-file) (`tools/generate-cat-file/`, GPLv2, build-time only). OGG Vorbis is decoded by [stb_vorbis](https://github.com/nothings/stb) (`app/third_party/stb_vorbis.c`, public domain); mp3, wav and flac by Windows Media Foundation.

The music for Play / Pause is in `media/mp3/` (synthetic tracks made for this project); add or remove music files there (mp3, wav, flac, ogg) — without any the package still builds and the button is disabled.

Other checks: `./check_syntax.sh` (driver sources against the MinGW DDK headers), `./check_inf.sh` (Microsoft InfVerif under Wine: basic, `/h` and `/w` modes).

### Alternative: WDK on Windows

1. Visual Studio 2022 ("Desktop development with C++") and the Windows Driver Kit matching the Windows SDK, with the WDK extension.
2. Open `driver\Speak2Mic.vcxproj`, `Release | x64`, Driver Signing → Sign Mode = **Test Sign**, Build.
3. Programs: `app\build.cmd` from the "x64 Native Tools Command Prompt for VS 2022".

Device names are generated: edit `gen.py` and run `python gen.py` (writes `driver\pinnames.h` and `driver\Speak2Mic.inf`).

## How it works

- `driver\adapter.cpp` — entry point: registers four PortCls filters for the cable (WaveRT + Topology for playback, the same for recording) and the physical connections between them; reads the settings.
- `driver\minwave.cpp` — WaveRT miniport. There is no hardware: the buffer position follows the performance counter at the cable's rate; on every position request of the Windows audio engine and every 5 ms by timer, "played" frames move from the playback buffer into the cable and "recorded" frames from the cable into the capture buffer.
- `driver\cable.cpp` — a one-second ring buffer addressed by absolute frame number. The capture side reads `LatencyMs` behind "now"; where nothing was written it returns silence. 16/24/32-bit PCM on either end in any combination, channel up/downmix, and the microphone volume/mute gain.
- `driver\mintopo.cpp` — topology: the playback end is a line out, the recording end a **microphone** with a volume node (−96…+9.5625 dB in 1/16 dB steps) and a mute node. The device names come from `MediaCategories` entries written by the INF.
- `driver\Speak2Mic.rc` — icons and version information (resource 100: Device Manager icon; 101/102: the speaker and microphone icons in the Sound settings; drawn by `app/make_icon.py`).
- `app\audio.cpp` — shared code: device list, default formats (`IPolicyConfig`), level meters, endpoint volume.
- `app\s2mpanel.cpp` — the panel: plain Win32, high DPI; settings are applied by restarting itself elevated (`Speak2Mic.exe --apply …`) and restarting the device through SetupAPI.

## Limitations

- Windows 10 2004 or later and Windows 11 (the driver uses `ExAllocatePool2`; the INF says 10.0.19041), x64 and x86.
- Test signing mode is required. Running without it needs a Microsoft signature (EV certificate + attestation signing in the Partner Center).
- One cable, one stream per end; in shared mode Windows mixes all applications anyway.
- No resampling in the driver: the cable has one sample rate (chosen in the panel).

## Logs and tools

- **Programs** write UTF-8 logs with timestamps to `C:\ProgramData\Speak2Mic\logs\` (`setup.log`, `panel.log`, `install.log`, `ctl.log`, `autotest.log`; over 1 MB → `*.old.log`); the panel's event list is kept in `events.log` (last 500 events). Each starts with the program, its build and the Windows version.
- **The driver** logs every step of its start and of its streams with NTSTATUS codes: in memory, to the kernel debugger (DebugView → Capture Kernel) and to `HKLM\SYSTEM\CurrentControlSet\Services\Speak2Mic\Parameters\DriverLog`.
- **Diagnostics:** `Speak2Mic-Setup.exe` → "Diagnostics" or `s2minstall.exe diag` check the device (Device Manager problem code), the driver service and the Speak2Mic sound devices, and add the driver log and the Speak2Mic part of `setupapi.dev.log`.
- **`s2mctl.exe`** — everything the panel does: `status`; `set [--preset voice|standard|high|studio|max] [--rate HZ] [--bits 0|16|24|32] [--channels N] [--mic-channels N] [--latency MS]` (administrator); `name [--speaker "NAME"] [--mic "NAME"]` (`default` = default name); `volume 0..300`; `mute on|off`; `reset` (administrator); `export FILE.ini` / `import FILE.ini` (import: administrator); `test [SECONDS]`. Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 administrator rights needed, 4 cannot work (Secure Boot on, test mode off or driver missing).
- **`s2mautotest.exe [minutes] [--seed N]`** (administrator, 10 minutes by default) closes the panel, saves the current state and exercises everything at random: random settings with device restarts, a tone through the cable on every speaker channel (arrival, clipping, dropouts, silence, latency), renaming, microphone volume and mute, default formats, stream open/close stress, the Test sound and the music player, leftover endpoint records and random `s2mctl` commands (including invalid ones and an export/import round trip). It restores everything at the end (also after Ctrl+C) and logs every check as PASS/FAIL/WARN; `--seed` repeats a run. It also logs outside volume changes, the audio sessions open on the microphone and the audio effects (APOs) on the devices.

## Troubleshooting

- **Code 52 in Device Manager (signature):** test signing mode is off or Secure Boot is on.
- **"Device not found" in the panel:** Device Manager → Sound, video and game controllers → Speak2Mic: is it started? The panel picks it up by itself within seconds.
- **The speaker meter moves, the microphone one does not:** "Speak2Mic Microphone" may be disabled or muted (Sound → Recording); the microphone meter also lags by `LatencyMs`.
- **The microphone volume keeps jumping to the maximum:** another program adjusts it (call apps with automatic microphone gain, monitoring agents). The panel names it in the event list.
- **Silence on the microphone in a VM** while the screen is off or the session is locked: Windows may hold back the sound of an inactive session; disable screen-off (`powercfg /change monitor-timeout-ac 0`).
- **Clicks or dropouts:** raise the latency (50–100 ms), especially in a VM under host load.
- **Blue screen:** roll the VM back to the snapshot and keep the dump (`C:\Windows\MEMORY.DMP` or `Minidump`).

## Languages

Русский, English, українська, беларуская, Deutsch, français, español, italiano, português (Brasil), polski, Nederlands, Türkçe, Bahasa Indonesia, Tiếng Việt, 中文(简体), 日本語, 한국어. The default is the Windows display language, then the regional settings, otherwise English; the "Language" list in the top right corner of the panel and the installer changes it for all programs (`HKCU\Software\Speak2Mic\Language`); console programs take `--lang <code>`.

The source strings in the code are Russian and serve as translation keys: `TR(L"…")`. Translations are line by line in `app\lang\<code>.txt`, in the order of `app\lang\keys.txt`. The first 247 lines are the interface; the rest are technical diagnostics in Russian and English only. `python3 app/lang/gen_lang.py` builds `app\lang_table.inc` and checks that every translation keeps the `%` conversions of its key. A new language: add it to `LANGS` in `gen_lang.py` and add its file.

## Author

Symo — symops@gmail.com
