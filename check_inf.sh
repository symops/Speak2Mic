#!/bin/sh
# Runs Microsoft InfVerif (from the WDK NuGet package microsoft.windows.wdk.x64, tools/infverif/infverif.exe) under
# Wine on the packaged INF. Usage: ./check_inf.sh [mode ...]   modes: "" (basic), /u, /k, /h (WHQL signature
# requirements - what a Microsoft signature needs), /w (Windows Driver, stricter). Default: basic, /h and /w.
cd "$(dirname "$0")"
INF=${INF:-dist/Speak2Mic-x64/Speak2Mic.inf}
command -v wine >/dev/null || { echo "wine is not installed (apt install wine)"; exit 2; }
[ -f tools/infverif/infverif.exe ] || { echo "tools/infverif/infverif.exe is missing"; exit 2; }
T=$(mktemp -d) && cp "$INF" "$T/Speak2Mic.inf" && cp tools/infverif/infverif.exe "$T/"
[ $# -eq 0 ] && set -- "" /h /w
rc=0
for mode in "$@"; do
    echo "=== infverif /v $mode"
    (cd "$T" && WINEDEBUG=-all wine infverif.exe /v $mode Speak2Mic.inf 2>&1) | tr -d '\r' |
        grep -v '^$\|Running in Verbose\|Checked' | sed 's|Z:.*\\Speak2Mic.inf|Speak2Mic.inf|'
    (cd "$T" && WINEDEBUG=-all wine infverif.exe $mode Speak2Mic.inf >/dev/null 2>&1) || rc=1
done
rm -rf "$T"
exit $rc
