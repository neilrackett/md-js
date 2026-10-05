#!/bin/sh
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MD/JS in Hatari, with EmuMD (emu/emumd): builds the firmware and
# FETCHTST.TOS (emu/test), serves pages from this computer (which the
# firmware reaches as 10.0.2.2), and checks what JavaScript's fetch() got:
# a small page, one bigger than lwIP's TCP window (which the firmware has
# to reopen as it reads), and a missing one (404). Then it boots again
# with WiFi slow to connect, and checks the ST finds MD/JS meanwhile.
# Needs stcmd (for the ST program), libslirp and python3.
#
#   emu/test.sh          (MDFW=.../tools/mdfw for another EmuMD)

set -eu
cd "$(dirname "$0")/.."
MDFW=${MDFW:-emu/emumd/tools/mdfw}

$MDFW build
STCMD_NO_TTY=1 ST_WORKING_FOLDER="$PWD" stcmd make -s -C emu/test >/dev/null

WORK=$(mktemp -d)
SERVER=
cleanup() {
    set +e
    [ -n "$SERVER" ] && kill "$SERVER" && wait "$SERVER" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir "$WORK/www" "$WORK/c"
printf 'Hello from this computer\n' > "$WORK/www/small.txt"
python3 -c 'import sys; sys.stdout.write(("0123456789abcdefghijklmnopqrstuvwxyz" * 28 + "\n") * 20)' \
    > "$WORK/www/big.txt"
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WORK/www" >/dev/null 2>&1 &
SERVER=$!
cp emu/test/dist/FETCHTST.TOS "$WORK/c/"

# st URLS [mdfw run options]: run FETCHTST.TOS on those URLs; its results.
st() {
    printf '%s' "$1" > "$WORK/c/URLS.TXT"
    rm -f "$WORK/c/RESULTS.TXT"
    shift
    $MDFW run --headless --frames 4000 --timeout 300 --harddrive "$WORK/c" \
        --log "$WORK/hatari.log" "$@" -- --auto 'C:\FETCHTST.TOS' >/dev/null
    [ -f "$WORK/c/RESULTS.TXT" ] ||
        { echo "test: FETCHTST.TOS wrote nothing"; tail -20 "$WORK/hatari.log"; exit 1; }
    tr -d '\r' < "$WORK/c/RESULTS.TXT" | tee "$WORK/results"
}

fail=0
check() {
    grep -qF "$1" "$WORK/results" || { echo "test: expected $1"; fail=1; }
}

URL=http://10.0.2.2:$PORT
st "$URL/small.txt
$URL/big.txt
$URL/missing.txt
"
check 'MD/JS found'
check 'small.txt {"ok":true,"status":200,"statusText":"OK","length":25,"start":"Hello from this "}'
# fetch() keeps the first 4 KB of a body.
check 'big.txt {"ok":true,"status":200,"statusText":"OK","length":4095,"start":"0123456789abcdef"}'
check 'missing.txt {"ok":false,"status":404,"statusText":"File not found"'
check 'done'

# WiFi taking 20 s to join: MD/JS answers the ST all the same.
st "" -O wifi_join_ms=20000
check 'MD/JS found'

[ $fail = 0 ] && echo "test: all good"
exit $fail
