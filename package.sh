#!/bin/sh
# Builds everything on Linux (clang + MinGW-w64, no WDK) and assembles one package for 64-bit and 32-bit Windows:
#   dist/Speak2Mic-Setup.exe        the release: ONE file - the launcher with the whole package below appended
#                                   (a cabinet, MSZIP; extracted to a temporary folder when it runs)
#   dist/Speak2Mic/                 the same package unpacked (for development / testing)
#     Speak2Mic-Setup.exe           x86 launcher: starts x64\ or x86\Speak2Mic-Setup.exe (whichever Windows this is)
#     uninstall.cmd                 the same for removal
#     mp3\                          music for the panel's "Play" button (installed to Program Files\Speak2Mic\mp3)
#     x64\  x86\                    the programs, the driver (.sys, .inf, .cat) and the test certificates of each
set -e
cd "$(dirname "$0")"
python3 gen.py
python3 app/lang/gen_lang.py
./check_syntax.sh
ARCH=x64 driver/mingw/build.sh
ARCH=x86 driver/mingw/build.sh
ARCH=x64 app/build.sh
ARCH=x86 app/build.sh

D=dist/Speak2Mic
rm -rf dist
mkdir -p $D/mp3
cp app/x86/s2mlauncher.exe $D/Speak2Mic-Setup.exe
cp scripts/uninstall.cmd $D/
# Music is optional (not in the repository): without mp3 files the panel's Play button is just disabled.
for f in media/mp3/*.mp3; do [ -e "$f" ] && cp "$f" $D/mp3/; done
make -s -C tools/generate-cat-file

for ARCH in x64 x86; do
    A=$D/$ARCH
    mkdir -p $A
    if [ $ARCH = x64 ]; then DRVOUT=driver/mingw/out; APP=app; MODEL=NTamd64; OSATTR=_v100_X64
    else DRVOUT=driver/mingw/out-x86; APP=app/x86; MODEL=NTx86; OSATTR=_v100; fi
    cp $DRVOUT/Speak2Mic.sys $DRVOUT/Speak2Mic.cer $DRVOUT/Speak2Mic-Publisher.cer $A/
    cp $APP/Speak2Mic.exe $APP/Speak2Mic-Setup.exe $APP/s2minstall.exe $APP/s2mautotest.exe $APP/s2mctl.exe $A/
    cp scripts/uninstall.cmd $A/

    # INF of this architecture: gen.py writes NTamd64 and NTarm64 models; keep NTamd64 (x64) or turn it into NTx86.
    python3 - "$A/Speak2Mic.inf" "$MODEL" <<'PY'
import sys
src = open('driver/Speak2Mic.inf', encoding='utf-8').read().replace('\r\n', '\n')
model = sys.argv[2]
out, skip = [], False
for line in src.split('\n'):
    if line.startswith('%MfgName%=Speak2Mic,'):
        line = line.split(',NTarm64')[0].replace('NTamd64', model)    # keeps the TargetOSVersion decoration
    if line.startswith('['):
        skip = line.strip().startswith('[Speak2Mic.NTarm64')
        if line.startswith('[Speak2Mic.NTamd64'):
            line = line.replace('NTamd64', model)
    if skip:
        continue
    out.append(line)
open(sys.argv[1], 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out))
PY

    # Catalog (.cat): SHA1 hashes of the final INF and SYS, required by Windows to add the package to the driver
    # store (error 0xE000022F without it). Made with LINBIT generate-cat-file, test-signed with the driver's
    # certificate. OS attribute: Windows 10 x64 (_v100_X64) or x86 (_v100).
    tools/generate-cat-file/gencat.sh -o $A/Speak2Mic.cat.unsigned -h 'root\speak2mic' \
        -O $OSATTR -A 2:10.0 -T "$(date -u +%y%m%d%H%M%SZ)" $A/Speak2Mic.inf $A/Speak2Mic.sys
    osslsigncode sign -certs driver/mingw/testcert/chain.pem -key driver/mingw/testcert/key.pem -h sha256 \
        -n "Speak2Mic" -in $A/Speak2Mic.cat.unsigned -out $A/Speak2Mic.cat >/dev/null
    rm -f $A/Speak2Mic.cat.unsigned
    osslsigncode verify -CAfile driver/mingw/testcert/root.pem -in $A/Speak2Mic.cat 2>&1 | grep -q "Signature verification: ok" \
        || { echo "catalog signature check failed ($ARCH)"; exit 1; }
    echo "catalog: $A/Speak2Mic.cat (signed)"
done

# Windows cmd.exe reads .cmd files in the OEM code page; convert the Russian text to CP866.
for f in $D/uninstall.cmd $D/x64/uninstall.cmd $D/x86/uninstall.cmd; do
    iconv -f UTF-8 -t CP866 "$f" | sed 's/$/\r/' > "$f.tmp" && mv "$f.tmp" "$f"
done

# One file: the launcher + the package as a cabinet (MSZIP; Windows extracts it with SetupIterateCabinet) + a trailer
# "S2MPAYLD" + the cabinet's size (little endian, 8 bytes). See app/s2mlauncher.cpp.
(cd $D && find uninstall.cmd mp3 x64 x86 -type f | sort | xargs gcab -c -z ../payload.cab)
python3 - <<'PY'
import struct
launcher = open('app/x86/s2mlauncher.exe', 'rb').read()
cab = open('dist/payload.cab', 'rb').read()
open('dist/Speak2Mic-Setup.exe', 'wb').write(launcher + cab + b'S2MPAYLD' + struct.pack('<Q', len(cab)))
PY
rm -f dist/payload.cab
ls -la dist/Speak2Mic-Setup.exe
