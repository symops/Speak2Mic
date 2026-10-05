#!/bin/sh
# Cross-builds the programs on Linux (clang + MinGW-w64): ARCH=x64 (default) into app/, ARCH=x86 into app/x86/.
# On Windows use build.cmd (MSVC) instead.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-x64}
if [ "$ARCH" = x86 ]; then TRIPLE=i686-w64-mingw32; TARGET=i686-w64-windows-gnu; O=x86
else TRIPLE=x86_64-w64-mingw32; TARGET=x86_64-w64-windows-gnu; O=.; fi
T=${TMPDIR:-/tmp}/s2mbuild.$$
mkdir -p "$T" "$O"
CXX="clang++ --target=$TARGET -std=c++17 -O2 -fno-exceptions -fno-rtti -municode
     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter
     -Wno-missing-field-initializers -isystem /usr/$TRIPLE/include"
$CXX -c audio.cpp -o "$T/audio.o"
$CXX -c applog.cpp -o "$T/applog.o"
$CXX -c lang.cpp -o "$T/lang.o"
$CXX -c audiosvc.cpp -o "$T/audiosvc.o"
$CXX -c diag.cpp -o "$T/diag.o"
$CXX -c ksprobe.cpp -o "$T/ksprobe.o"
$CXX -c s2mpanel.cpp -o "$T/s2mpanel.o"
$CXX -c mp3player.cpp -o "$T/mp3player.o"
$TRIPLE-windres s2mpanel.rc -O coff -o "$T/s2mpanel.res.o"
$CXX -c s2minstall.cpp -o "$T/s2minstall.o"
$CXX -c setupcore.cpp -o "$T/setupcore.o"
$CXX -c setupfiles.cpp -o "$T/setupfiles.o"
$CXX -c s2msetup.cpp -o "$T/s2msetup.o"
$CXX -c devctl.cpp -o "$T/devctl.o"
$CXX -c s2mautotest.cpp -o "$T/s2mautotest.o"
$CXX -c s2mctl.cpp -o "$T/s2mctl.o"
$TRIPLE-windres s2mctl.rc -O coff -o "$T/s2mctl.res.o"
$TRIPLE-windres s2mautotest.rc -O coff -o "$T/s2mautotest.res.o"
$TRIPLE-windres s2msetup.rc -O coff -o "$T/s2msetup.res.o"
$TRIPLE-windres s2minstall.rc -O coff -o "$T/s2minstall.res.o"
$TRIPLE-gcc -municode -mwindows -static -s -o "$O/Speak2Mic.exe" "$T/s2mpanel.o" "$T/mp3player.o" "$T/audio.o" "$T/applog.o" "$T/lang.o" "$T/audiosvc.o" "$T/devctl.o" "$T/s2mpanel.res.o" \
    -lole32 -lavrt -luuid -lcomctl32 -lcomdlg32 -lsetupapi -lshell32 -lgdi32 -ladvapi32 -luser32 -lmfplat -lmfreadwrite -lmfuuid -lpropsys -lbcrypt
$TRIPLE-gcc -municode -static -s -o "$O/s2minstall.exe" "$T/s2minstall.o" "$T/audiosvc.o" "$T/devctl.o" "$T/setupcore.o" "$T/setupfiles.o" "$T/diag.o" "$T/ksprobe.o" "$T/applog.o" "$T/lang.o" "$T/s2minstall.res.o" -lwevtapi -lsetupapi -lnewdev -lcrypt32 -ladvapi32 -lole32 -lshell32 -luuid
$TRIPLE-gcc -municode -mwindows -static -s -o "$O/Speak2Mic-Setup.exe" "$T/s2msetup.o" "$T/audiosvc.o" "$T/devctl.o" "$T/setupcore.o" "$T/setupfiles.o" "$T/diag.o" "$T/ksprobe.o" "$T/applog.o" "$T/lang.o" "$T/s2msetup.res.o" \
    -lsetupapi -lnewdev -lcrypt32 -lcomctl32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -lwevtapi -luuid
$TRIPLE-gcc -municode -static -s -o "$O/s2mautotest.exe" "$T/s2mautotest.o" "$T/mp3player.o" "$T/audio.o" "$T/audiosvc.o" "$T/devctl.o" "$T/setupcore.o" "$T/setupfiles.o" "$T/diag.o" "$T/ksprobe.o" "$T/applog.o" "$T/lang.o" "$T/s2mautotest.res.o" \
    -lwevtapi -lsetupapi -lnewdev -lcrypt32 -ladvapi32 -lole32 -lshell32 -luuid -lavrt -lpropsys -lwtsapi32 -lmfplat -lmfreadwrite -lmfuuid -lbcrypt
$TRIPLE-gcc -municode -static -s -o "$O/s2mctl.exe" "$T/s2mctl.o" "$T/audio.o" "$T/audiosvc.o" "$T/devctl.o" "$T/setupcore.o" "$T/setupfiles.o" "$T/diag.o" "$T/ksprobe.o" "$T/applog.o" "$T/lang.o" "$T/s2mctl.res.o" \
    -lwevtapi -lsetupapi -lnewdev -lcrypt32 -ladvapi32 -lole32 -lshell32 -luuid -lavrt -lpropsys
if [ "$ARCH" = x86 ]; then
    # The package's root Speak2Mic-Setup.exe: starts x64\ or x86\Speak2Mic-Setup.exe (see s2mlauncher.cpp).
    $CXX -c s2mlauncher.cpp -o "$T/s2mlauncher.o"
    $TRIPLE-windres s2mlauncher.rc -O coff -o "$T/s2mlauncher.res.o"
    $TRIPLE-gcc -municode -mwindows -static -s -o "$O/s2mlauncher.exe" "$T/s2mlauncher.o" "$T/lang.o" "$T/applog.o" "$T/s2mlauncher.res.o" \
        -lsetupapi -lshell32 -luser32 -ladvapi32 -lole32
fi
rm -rf "$T"
echo "built ($ARCH, $O): Speak2Mic.exe Speak2Mic-Setup.exe s2minstall.exe s2mautotest.exe s2mctl.exe"
