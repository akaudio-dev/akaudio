#!/bin/bash
# Liveness sweep over the bundled Radio station presets.
#
# Drives every presets/Radio/**/*.vcvm URL through test/play_test — i.e. checks
# that real audio DECODES (the repo rule for shipping a station), not merely
# that the host connects. Run before a Library release; niche feeds rot.
#
# Usage:  tools/check_stations.sh [seconds-per-station]     (default 8)
#         JOBS=n tools/check_stations.sh                    (default 4 parallel)
#
# Needs build/play_test — build it with the compile line in CLAUDE.md
# ("Test harnesses"). Exits non-zero if any station fails.
set -u
cd "$(dirname "$0")/.."

SECS=${1:-8}
JOBS=${JOBS:-4}
PLAY=$PWD/build/play_test

if [ ! -x "$PLAY" ]; then
	echo "error: $PLAY not found — build the play_test harness first (see CLAUDE.md)" >&2
	exit 2
fi

check_one() {
	local f=$1
	local url name out
	url=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["data"]["url"])' "$f")
	name=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["data"]["stationName"])' "$f")
	out=$("$PLAY" "$url" "$SECS" 2>&1)
	if printf '%s' "$out" | grep -q '^PASS'; then
		printf 'PASS\t%s\t%s\n' "$name" "$url"
		return
	fi
	# Scanner/ATC feeds are silent between transmissions: frames decode at full
	# rate but happen to be all-zero in this window. That's alive, not rot.
	local pulled
	pulled=$(printf '%s' "$out" | awk '/Frames pulled/ {print $4}')
	if [ "${pulled:-0}" -gt 10000 ]; then
		printf 'SILENT\t%s\t%s\n' "$name" "$url"
		return
	fi
	# Last status line before the verdict is the most useful failure hint
	# (e.g. "HTTP: HTTP/1.1 301 Moved Permanently", "Cannot resolve host").
	local hint
	hint=$(printf '%s' "$out" | grep -Eo '\] .*' | tail -1 | cut -c3- | cut -c1-60)
	printf 'FAIL\t%s\t%s\t%s\t%s\n' "$name" "$url" "$f" "$hint"
}
export -f check_one
export PLAY SECS

echo "Checking $(find presets/Radio -name '*.vcvm' | wc -l | tr -d ' ') stations, ${SECS}s each, ${JOBS} at a time…"
echo

results=$(find presets/Radio -name '*.vcvm' -print0 | sort -z \
	| xargs -0 -P "$JOBS" -I{} bash -c 'check_one "$@"' _ {})

printf '%s\n' "$results" | awk -F'\t' '$1=="PASS" {printf "  ✓ %-28s %s\n", $2, $3}'
printf '%s\n' "$results" | awk -F'\t' '$1=="SILENT" {printf "  ~ %-28s %s   (decoding, silent this window)\n", $2, $3}'
fails=$(printf '%s\n' "$results" | awk -F'\t' '$1=="FAIL"')
echo
if [ -n "$fails" ]; then
	echo "FAILURES:"
	printf '%s\n' "$fails" | awk -F'\t' '{printf "  ✕ %-28s %s\n      %s\n      last status: %s\n", $2, $3, $4, $5}'
fi
npass=$(printf '%s\n' "$results" | grep -c '^PASS')
nsilent=$(printf '%s\n' "$results" | grep -c '^SILENT')
nfail=$(printf '%s\n' "$results" | grep -c '^FAIL')
echo
echo "$npass passed, $nsilent silent-but-alive, $nfail failed"
[ "$nfail" -eq 0 ]
