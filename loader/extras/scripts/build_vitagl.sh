#!/bin/sh
set -eu

VITAGL_DIR=$1; shift
NPROC=$1; shift
FLAGS="$*"
STAMP="$VITAGL_DIR/.vitagl_flags.stamp"

OLD=""
[ -f "$STAMP" ] && OLD=$(cat "$STAMP")

if [ "$OLD" != "$FLAGS" ]; then
    echo "vitaGL: flags changed, rebuilding from scratch"
    find "$VITAGL_DIR/source" -name '*.o' -delete 2>/dev/null || true
    rm -f "$VITAGL_DIR/libvitaGL.a"
    rm -f "$STAMP"
fi

make -C "$VITAGL_DIR" -j"$NPROC" $FLAGS
printf '%s' "$FLAGS" > "$STAMP"
