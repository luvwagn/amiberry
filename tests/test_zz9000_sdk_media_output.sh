#!/usr/bin/env bash
# ZZ9K_VIDEO_OUTPUT_SHARED_BUFFER media-session output.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${TMPDIR:-/tmp}/zz9000_sdk_media_output_test.$$"
mpeg1="${TMPDIR:-/tmp}/zz9000_sdk_media_output_test.$$.mpg"
mpeg2="${TMPDIR:-/tmp}/zz9000_sdk_media_output_test.$$.vob"
trap 'rm -f "$out" "$mpeg1" "$mpeg2"' EXIT
ffmpeg -nostdin -loglevel error -y -f lavfi -i 'testsrc2=size=160x120:rate=25:duration=1' -c:v mpeg1video -q:v 4 -an -f mpeg "$mpeg1"
ffmpeg -nostdin -loglevel error -y -f lavfi -i 'testsrc2=size=160x120:rate=25:duration=1' -c:v mpeg2video -pix_fmt yuv420p -an -f vob "$mpeg2"
. tests/lib/zz9000_sdk_build.sh
zz9000_sdk_compile "$out" tests/zz9000_sdk_media_output_test.cpp
"$out" "$mpeg1" "$mpeg2"
