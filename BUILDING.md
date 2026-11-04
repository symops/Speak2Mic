# Building Speak2Mic from a clean Debian 13

This describes building **Speak2Mic** (repo `symops/Speak2Mic`) — the virtual audio cable driver (a speaker whose sound
comes out of a microphone), the control panel, the installer and the tools, for 64-bit and 32-bit Windows — from
scratch on a fresh Debian 13 ("trixie") machine. Everything is built on Linux with clang and MinGW-w64: no Windows,
Visual Studio or WDK is needed (`driver/Speak2Mic.vcxproj` is kept for building it there too), and the driver is
test-signed with a certificate the build creates itself.

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git clang mingw-w64 gcc make libc6-dev \
    osslsigncode openssl gcab python3 python3-pil \
    ffmpeg
```

Checked with exactly these packages in a clean `debian:trixie` container (clang 19, MinGW-w64 12): a full
`./package.sh` without a single warning. Newer toolchains work as well (Ubuntu 26.04: clang 21, a newer MinGW-w64).

What each part is for:

| Packages | Used for |
|---|---|
| `clang`, `mingw-w64` | the driver and the programs (clang compiles against the MinGW-w64 headers, its DDK headers included; the MinGW `gcc`, `windres`, `ar`, `nm` and `dlltool` link, add resources and make the kernel import libraries) — both `x86_64` and `i686` |
| `gcc`, `make`, `libc6-dev` | `tools/generate-cat-file`, a small host program that writes the driver's security catalog (`.cat`) |
| `osslsigncode`, `openssl` | the test certificates, the signature of `Speak2Mic.sys` and of the catalog |
| `gcab` | the cabinet inside the one-file installer |
| `python3`, `python3-pil` | `gen.py` (version, INF), the language tables, the INF variants and the autotest's samples |
| `ffmpeg` | the autotest's sample music in the formats Windows cannot write itself (MP3, Ogg Vorbis, FLAC), made anew on every build, see step 6 |

Optional, only for `./check_inf.sh` (Microsoft's INF checker, step 7): `sudo apt install -y wine`.

## 2. Clone the repository

```sh
git clone git@github.com:symops/Speak2Mic.git
cd Speak2Mic
```

`app/third_party/stb_vorbis.c` (the Ogg Vorbis decoder of the music player, public domain) is in the repository:
there is nothing else to fetch.

## 3. The test signing keys

The driver has no Microsoft signature: Windows loads it only in **test signing mode** and only when it trusts the
certificate it was signed with. `driver/mingw/build.sh` creates that certificate on the first build when
`driver/mingw/testcert/` is missing:

- `root.pem` / `root.key` — "Speak2Mic Test Root CA", valid 10 years; the installer puts it into the machine's
  *Trusted Root* store (`Speak2Mic.cer` in the package),
- `cert.pem` / `key.pem` — "Speak2Mic Test Signing", issued by that root, which signs `Speak2Mic.sys` and the
  catalog; the installer puts it into *Trusted Publishers* (`Speak2Mic-Publisher.cer`),
- `chain.pem` — both, for `osslsigncode`.

**Never publish `driver/mingw/testcert/`** (it is in `.gitignore`): every machine the installer ran on trusts that
root for anything signed with it. **Keep it** between builds: an update signed with the same root installs over the
old version without a new certificate prompt. A build machine without the folder makes new keys; installers built
from them still work (they add their own root), the machines just trust one more test root.

## 4. Build

```sh
./package.sh
```

One command builds and packages everything; it takes a few minutes. In order:

1. `gen.py` — a new version **1.0.\<day of year\>.\<minute of day\>** into `driver/version.h` (programs, version
   resources, installer) and the INF's `DriverVer`. Every build must have a new one: Windows identifies a driver
   package by its INF and treats an INF with an unchanged `DriverVer` as "already imported" — the old driver would
   keep running.
2. `app/lang/gen_lang.py` — the 17 interface languages into `app/lang_table.inc`.
3. `check_syntax.sh` — a syntax and type check of the driver sources against the MinGW DDK headers.
4. `driver/mingw/build.sh` for x64 and x86 — `Speak2Mic.sys` (a PortCls WaveRT miniport): compiled against the
   MinGW DDK headers, linked with import libraries generated from what the objects import (`portcls.sys`,
   `ntoskrnl.exe`, `hal.dll`), the PE header fixed for a kernel driver (`pefix.py`), test-signed.
5. `app/build.sh` for x64 and x86 — the programs and the autotest's samples (step 6) on the x64 pass.
6. `tools/generate-cat-file` — the catalog of `Speak2Mic.inf` + `Speak2Mic.sys` for each architecture, test-signed
   and checked (`osslsigncode verify`); Windows refuses a driver package without a catalog (`0xE000022F`), also in
   test signing mode.
7. The two packages and their one-file installers (step 5 below).

`set -e` stops at the first error. Warnings are not expected: a build is clean when
`grep -iE "error|warning:"` finds nothing in its output.

## 5. What comes out

| Path | Contents |
|---|---|
| `dist/Speak2Mic-Setup.exe` | **the release**, one file: the 32-bit launcher with the package below appended as a cabinet; it unpacks itself to a temporary folder, starts the installer of this Windows' bitness and cleans up |
| `dist/Speak2Mic/` | the same unpacked: `Speak2Mic-Setup.exe` (launcher) and `x64\`, `x86\` — `Speak2Mic-Setup.exe` (installer), `Speak2Mic.exe` (control panel), `Speak2Mic.sys`, `Speak2Mic.inf`, `Speak2Mic.cat`, `Speak2Mic.cer`, `Speak2Mic-Publisher.cer` |
| `dist/Speak2Mic-Debug-Setup.exe`, `dist/Speak2Mic-Debug/` | **for testing only**: the same build plus `uninstall.cmd`, `s2minstall.exe` (console installer and diagnostics), `s2mctl.exe` (command line control) and `s2mautotest.exe` (autotest) |

Give users the release. The debug package installs the same product plus the tools; the release installed over it
removes them again (the installer deletes a file of its list that its package does not have).

Music for the panel's **Play** button is not in the repository: MP3 files put into `media/mp3/` before the build go
into the packages' `mp3\` folder and are installed next to the program; without them the panel offers its generator
and the test sound. Windows on ARM is not supported.

## 6. The autotest's samples

`app/build.sh` runs `tools/make_autotest_media.py` on every x64 build: a melody in a new random order each time,
encoded as Ogg Vorbis (stereo and mono), MP3 (CBR, VBR with ID3 tags, mono 22 kHz, with an attached cover picture),
FLAC 96 kHz and WAV (8, 24 bit, float). They go to `app/autotest-media/` and `app/autotest_media.rc` (both ignored by
git) and are built into `s2mautotest.exe` only. The x86 pass reuses the same set. Broken files are **not** among
them: the autotest makes those itself when it starts.

If `ffmpeg` is missing, install it (step 1) rather than skip the samples: the autotest would then not test those
formats.

## 7. Check the INF (optional)

```sh
./check_inf.sh
```

runs Microsoft's **InfVerif** under Wine on the packaged INF (basic, `/h` and `/w` by default). InfVerif is not
redistributable: take `infverif.exe` from the WDK NuGet package `microsoft.windows.wdk.x64`
(`c/tools/<version>/x64/infverif.exe`) and put it into `tools/infverif/`. Expected: basic and `/h` pass; `/w` reports
only the pin names under `HKLM\...\MediaCategories` (1321), which a WaveRT driver needs.

## 8. Install for testing

On the test machine (a VM is best):

1. **Secure Boot off** (VMware: VM → Settings → Options → Advanced → clear "Enable UEFI Secure Boot"; Hyper-V:
   Security → clear "Enable Secure Boot"). Windows does not load test-signed drivers with it.
2. Run `Speak2Mic-Setup.exe` (or `Speak2Mic-Debug-Setup.exe`). If test signing is off, click **Enable test mode**,
   restart, run it again, then **Install**. In a Remote Desktop session the sound has to play on the remote
   computer (RDP → Local Resources → Remote audio), otherwise Windows hides the Speak2Mic devices.

Without a window, for scripts (administrator):

```bat
Speak2Mic-Setup.exe /install     & rem exit code: 0 works, 1 failed, 2 restart needed,
                                 & rem 3 cannot install here (test signing off / Secure Boot on), 4 installed but not working
Speak2Mic-Setup.exe /remove      & rem exit code: 0 removed, 1 failed, 2 restart needed
Speak2Mic-Setup.exe /install /dir "D:\Tools"  & rem into D:\Tools\Speak2Mic (as "Browse..."; an installation stays where it is)
```

The installed product appears in Windows' **Apps / Programs and Features**; its Uninstall opens the installer.
Logs: `C:\ProgramData\Speak2Mic\logs\` (`setup.log`, `panel.log`, `events.log`, and in the debug package
`install.log`, `ctl.log`, `autotest.log`); the driver's own log is in
`HKLM\SYSTEM\CurrentControlSet\Services\Speak2Mic\Parameters\DriverLog`.

## 9. Run the autotest

From the **debug** package, in an administrator command prompt on the test machine:

```bat
"C:\Program Files\Speak2Mic\s2mautotest.exe" 120           & rem minutes (default 10); --seed N repeats a run
```

It closes the control panel, saves the current settings, runs random actions for the given time (settings with
device restarts, a tone through the cable on every speaker channel, the music player, volume, names, the command
line, settings export / import) and restores everything at the end (also after Ctrl+C). Every check is logged as
PASS / FAIL / WARN to `autotest.log`; the exit code is 1 if anything failed. Dropouts during a stall of the PC (a VM
whose host takes its CPUs away) or with the CPUs 80 % busy or more are warnings, not failures; the summary at the
end gives the totals.

## 10. Code checks

What the code is checked with besides the build, and how to repeat it (all on Linux; the results of the last run: no
compiler warning, CodeQL must-fix clean).

1. **Compiler warnings.** `./package.sh` builds everything with `-Wall -Wextra`, x64 and x86; a clean build prints no
   `warning:` (see step 4).
2. **Clang Static Analyzer** (the programs): for every `app/*.cpp`, with the build's flags,
   ```sh
   cd app
   for f in *.cpp; do
     clang++ --target=x86_64-w64-windows-gnu -std=c++17 -fno-exceptions -fno-rtti -municode -D_WIN32_WINNT=0x0A00 \
       -DWINVER=0x0A00 -DUNICODE -D_UNICODE -isystem /usr/x86_64-w64-mingw32/include -I../third_party/libyuv/include \
       --analyze -Xclang -analyzer-output=text -o /dev/null "$f"
   done
   ```
   The findings left are false positives (memory freed by the window a message is posted to, a window's state freed in
   `WM_NCDESTROY`).
3. **CodeQL with Microsoft's driver rules** - the static analysis of the Windows Hardware Compatibility Program
   ("Static Tools Logo"): CodeQL CLI 2.25.5 (github.com/github/codeql-cli-binaries) and the query packs
   `microsoft/windows-drivers@1.10.0` + `microsoft/cpp-queries@0.0.5` (`codeql pack download ...`). The build command goes
   in a script (CodeQL splits `--command` on spaces):
   ```sh
   printf '#!/bin/sh\nARCH=x64 exec sh driver/mingw/build.sh\n' > /tmp/drv.sh && chmod +x /tmp/drv.sh
   codeql database create /tmp/db --overwrite --language=cpp --source-root=. --command=/tmp/drv.sh
   codeql database analyze /tmp/db microsoft/windows-drivers:windows-driver-suites/mustfix.qls \
     --format=sarifv2.1.0 --output=speak2mic-mustfix.sarif
   ```
   Expected: no result for `mustfix.qls`; `recommended.qls` reports only false positives ("AddDevice does not clear DO_DEVICE_INITIALIZING": PortCls's `PcAddAdapterDevice` does it).
4. **InfVerif**: `./check_inf.sh` (step 7; `/w` reports only the MediaCategories pin names).

Not run here: Static Driver Verifier and Code Analysis (`/analyze`) need Windows with the WDK and Visual Studio; the
HLK tests need a physical test computer.

## Author

Symo &lt;symops@gmail.com&gt;
