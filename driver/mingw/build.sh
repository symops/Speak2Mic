#!/bin/sh
# Builds Speak2Mic.sys on Linux with clang + MinGW-w64 (no WDK), test-signs it and assembles
# a ready-to-install package in driver/mingw/out/.
# The official build path is still Visual Studio + WDK (driver/Speak2Mic.vcxproj).
set -e
cd "$(dirname "$0")"
# ARCH=x64 (default) -> out/ ; ARCH=x86 -> out-x86/ (32-bit Windows 10).
ARCH=${ARCH:-x64}
DRV=..
OBJ=${TMPDIR:-/tmp}/s2mdrv.$$
if [ "$ARCH" = x86 ]; then
    OUT=out-x86
    TRIPLE=i686-w64-mingw32
    ARCHFLAGS="--target=i686-w64-windows-gnu -D_X86_=1 -Di386=1"
else
    OUT=out
    TRIPLE=x86_64-w64-mingw32
    ARCHFLAGS="--target=x86_64-w64-windows-gnu -mno-red-zone -D_AMD64_ -D_WIN64"
fi
MINGW=/usr/$TRIPLE/include
mkdir -p "$OBJ" "$OUT"

CXXFLAGS="$ARCHFLAGS -std=c++17 -O2 -fms-extensions -fno-exceptions -fno-rtti
  -fno-stack-protector -fno-threadsafe-statics -fno-use-cxa-atexit
  -DNTDDI_VERSION=0x0A00000C -D_WIN32_WINNT=0x0A00
  -include $DRV/check_prelude.h -isystem $MINGW/ddk -isystem $MINGW -I$DRV
  -Wall -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-microsoft-anon-tag -Wno-nonportable-include-path"

for f in adapter cable minwave mintopo guids log; do
    clang++ $CXXFLAGS -c $DRV/$f.cpp -o "$OBJ/$f.o"
done
$TRIPLE-windres -c 65001 -I$DRV $DRV/Speak2Mic.rc -O coff -o "$OBJ/resources.o"
clang++ $CXXFLAGS -Wno-missing-braces -c stdunk_impl.cpp -o "$OBJ/stdunk_impl.o"
clang++ $CXXFLAGS -Wno-missing-braces -c ksguids.cpp -o "$OBJ/ksguids.o"

if [ "$ARCH" = x86 ]; then
    # 32-bit kernel imports carry their calling convention in the name (_Name@N stdcall, @Name@N fastcall, _Name
    # cdecl), and some live in hal.dll (spin locks, performance counter, IRQL). Generate the .def files from what the
    # objects import, then import libraries that bind to the undecorated export names (dlltool -k).
    $TRIPLE-nm -u "$OBJ"/*.o | sed -n 's/^ *U //p' | sort -u | python3 -c "
import sys
# the C runtime functions the compiler calls directly (no __imp_ prefix) also come from ntoskrnl.exe
crt = {'_memset', '_memcpy', '_memmove', '__snprintf', '__vsnprintf', '_strlen', '_wcslen'}
syms = set()
for s in sys.stdin.read().split():
    if s.startswith('__imp_'): syms.add(s[6:])
    elif s in crt: syms.add(s)
hal = {'KfAcquireSpinLock', 'KfReleaseSpinLock', 'KeQueryPerformanceCounter', 'KeGetCurrentIrql', 'KfRaiseIrql', 'KfLowerIrql'}
portcls = lambda n: n.startswith('Pc')
defs = {'ntoskrnl.exe': [], 'hal.dll': [], 'portcls.sys': []}
for sym in sorted(syms):
    if sym.startswith('@'): name, decorated = sym[1:].split('@')[0], sym           # fastcall
    else: decorated = sym[1:]; name = decorated.split('@')[0]                       # stdcall / cdecl
    dll = 'hal.dll' if name in hal else 'portcls.sys' if portcls(name) else 'ntoskrnl.exe'
    defs[dll].append(decorated)
for dll, names in defs.items():
    with open('$OBJ/' + dll.split('.')[0] + '_x86.def', 'w') as f:
        f.write('LIBRARY ' + dll + '\\nEXPORTS\\n' + ''.join(n + '\\n' for n in names))
"
    $TRIPLE-dlltool -k -d "$OBJ/portcls_x86.def" -D portcls.sys -l "$OBJ/libportcls.a"
    $TRIPLE-dlltool -k -d "$OBJ/ntoskrnl_x86.def" -D ntoskrnl.exe -l "$OBJ/libntoskrnl_vc.a"
    $TRIPLE-dlltool -k -d "$OBJ/hal_x86.def" -D hal.dll -l "$OBJ/libhal_vc.a"
    LIBGCC=$($TRIPLE-gcc -print-libgcc-file-name)       # 64-bit division (__udivdi3, __umoddi3)
    $TRIPLE-gcc -nostdlib -shared \
        -Wl,--subsystem,native -Wl,--entry,_DriverEntry@8 -Wl,--image-base,0x10000 \
        -Wl,--dynamicbase -Wl,--nxcompat \
        -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--no-insert-timestamp \
        -Wl,--exclude-all-symbols -Wl,--major-subsystem-version,10 -Wl,--minor-subsystem-version,0 \
        -Wl,--major-os-version,10 -Wl,--minor-os-version,0 \
        -o "$OBJ/Speak2Mic.sys" "$OBJ"/*.o -L"$OBJ" -lportcls -lntoskrnl_vc -lhal_vc "$LIBGCC"
else
    # Import libraries for exports missing from MinGW.
    $TRIPLE-dlltool -d portcls.def -D portcls.sys -l "$OBJ/libportcls.a"
    $TRIPLE-dlltool -d ntoskrnl.def -D ntoskrnl.exe -l "$OBJ/libntoskrnl_vc.a"

    $TRIPLE-gcc -nostdlib -shared \
        -Wl,--subsystem,native -Wl,--entry,DriverEntry -Wl,--image-base,0x140000000 \
        -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
        -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--no-insert-timestamp \
        -Wl,--exclude-all-symbols -Wl,--major-subsystem-version,10 -Wl,--minor-subsystem-version,0 \
        -Wl,--major-os-version,10 -Wl,--minor-os-version,0 \
        -o "$OBJ/Speak2Mic.sys" "$OBJ"/*.o -L"$OBJ" -lportcls -lntoskrnl_vc
fi

python3 pefix.py "$OBJ/Speak2Mic.sys"
rm -f "$OUT/Speak2Mic.unsigned.sys"

# Test certificates (created once, reused so Windows keeps trusting the same publisher):
#   root.pem  - "Speak2Mic Test Root CA" (CA:TRUE)      -> machine "Root" store      (Speak2Mic.cer)
#   cert.pem  - "Speak2Mic Test Signing" (CA:FALSE,     -> "TrustedPublisher" store  (Speak2Mic-Publisher.cer)
#               codeSigning), issued by the root; signs the .sys and the .cat
# Authenticode rejects a signer that is itself a CA (0x80096019), hence the two certificates.
CERT=testcert
if [ ! -f $CERT/root.pem ] || [ ! -f $CERT/cert.pem ]; then
    rm -rf $CERT && mkdir -p $CERT
    cat > $CERT/root.ext <<'X'
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
X
    cat > $CERT/leaf.ext <<'X'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=codeSigning
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
X
    openssl req -new -newkey rsa:2048 -nodes -keyout $CERT/root.key -subj "/CN=Speak2Mic Test Root CA" \
        -out $CERT/root.csr 2>/dev/null
    openssl x509 -req -in $CERT/root.csr -signkey $CERT/root.key -sha256 -days 3650 -extfile $CERT/root.ext \
        -out $CERT/root.pem 2>/dev/null
    openssl req -new -newkey rsa:2048 -nodes -keyout $CERT/key.pem -subj "/CN=Speak2Mic Test Signing" \
        -out $CERT/leaf.csr 2>/dev/null
    openssl x509 -req -in $CERT/leaf.csr -CA $CERT/root.pem -CAkey $CERT/root.key -CAcreateserial -sha256 \
        -days 3650 -extfile $CERT/leaf.ext -out $CERT/cert.pem 2>/dev/null
    rm -f $CERT/*.csr $CERT/*.srl
fi
cat $CERT/cert.pem $CERT/root.pem > $CERT/chain.pem
openssl x509 -in $CERT/root.pem -outform DER -out "$OUT/Speak2Mic.cer"
openssl x509 -in $CERT/cert.pem -outform DER -out "$OUT/Speak2Mic-Publisher.cer"

rm -f "$OUT/Speak2Mic.sys"
osslsigncode sign -certs $CERT/chain.pem -key $CERT/key.pem -h sha256 -n "Speak2Mic" \
    -in "$OBJ/Speak2Mic.sys" -out "$OUT/Speak2Mic.sys" >/dev/null
python3 pefix.py "$OUT/Speak2Mic.sys" --checksum-only
rm -rf "$OBJ"
echo "built and test-signed: $OUT/Speak2Mic.sys"
