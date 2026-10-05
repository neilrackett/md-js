#!/bin/sh
# Copyright (C) 2026 Neil Rackett
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MD/JS's fetch() in Hatari, with EmuMD (emu/emumd): builds the firmware
# and FETCHTST.TOS (emu/test), serves two pages from this computer (which
# the firmware reaches as 10.0.2.2), has JavaScript on the worker fetch
# them, and checks what came back. One page is bigger than lwIP's TCP
# window, which the firmware has to reopen as it reads. Needs stcmd (for
# the ST program, built once), libslirp and python3.
#
#   emu/test.sh          (MDFW=.../tools/mdfw for another EmuMD)

set -eu
cd "$(dirname "$0")/.."
MDFW=${MDFW:-emu/emumd/tools/mdfw}

$MDFW build
[ -f emu/test/dist/FETCHTST.TOS ] ||
    STCMD_NO_TTY=1 ST_WORKING_FOLDER="$PWD" stcmd make -C emu/test

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
printf 'http://10.0.2.2:%s/small.txt\nhttp://10.0.2.2:%s/big.txt\n' "$PORT" "$PORT" \
    > "$WORK/c/URLS.TXT"
cp emu/test/dist/FETCHTST.TOS "$WORK/c/"

$MDFW run --headless --frames 4000 --timeout 300 --harddrive "$WORK/c" \
    --log "$WORK/hatari.log" -- --auto 'C:\FETCHTST.TOS' >/dev/null

RESULTS="$WORK/c/RESULTS.TXT"
[ -f "$RESULTS" ] || { echo "test: FETCHTST.TOS wrote nothing"; tail -20 "$WORK/hatari.log"; exit 1; }
tr -d '\r' < "$RESULTS"
fail=0
check() {
    grep -q "$1" "$RESULTS" || { echo "test: expected $1"; fail=1; }
}
check 'small.txt {"ok":true,"status":200,"length":25,"start":"Hello from this "}'
# fetch() keeps the first 4 KB of a body.
check 'big.txt {"ok":true,"status":200,"length":4095,"start":"0123456789abcdef"}'
check '^done'
[ $fail = 0 ] && echo "test: fetch() works"
exit $fail
