#!/usr/bin/env bash
# Replay long BRICK sessions natively and fail on the states that lock up the demo:
# a ball frozen in place, a ball off the panel, or a round that stops progressing.
#
#   src/app/tools/test_brick.sh [seeds] [ticks]
set -euo pipefail
cd "$(dirname "$0")/../../.."   # keyboards/ydkb/athena75_rgb_advanced

SEEDS=${1:-${SEEDS:-80}}
TICKS=${2:-${TICKS:-400000}}
BIN=$(mktemp -u /tmp/brick_test.XXXXXX)

gcc -O1 -g -fsanitize=undefined -fno-sanitize-recover=all \
    -I src/app/sdk -o "$BIN" src/app/brick/brick_test.c

fail=0
for i in $(seq 1 "$SEEDS"); do
  seed=$((i * 2654435761 % 4294967291))
  if ! "$BIN" "$TICKS" "$seed"; then
    echo "--- seed $seed FAILED ---"
    fail=$((fail + 1))
    [ "$fail" -ge 3 ] && break
  fi
done

rm -f "$BIN"
if [ "$fail" -ne 0 ]; then
  echo "$fail seed(s) failed"
  exit 1
fi
echo "all $SEEDS seeds clean ($TICKS ticks each)"
