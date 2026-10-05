#!/bin/sh
# Syntax/type check of the driver sources against the MinGW DDK headers (Linux, no WDK).
# This is NOT a build: the real driver is built with Visual Studio + WDK on Windows.
cd "$(dirname "$0")/driver" || exit 1
MINGW=/usr/x86_64-w64-mingw32/include
rc=0
for f in *.cpp; do
  clang++ --target=x86_64-w64-windows-gnu -std=c++17 -fsyntax-only -fms-extensions -fno-exceptions -fno-rtti \
    -D_AMD64_ -D_WIN64 -DNTDDI_VERSION=0x0A00000C -D_WIN32_WINNT=0x0A00 \
    -include check_prelude.h -isystem $MINGW/ddk -isystem $MINGW -Wall -Wno-unknown-pragmas -Wno-ignored-attributes \
    -Wno-microsoft-anon-tag -Wno-nonportable-include-path "$f" || rc=1
done
[ $rc = 0 ] && echo "syntax check OK"
exit $rc
