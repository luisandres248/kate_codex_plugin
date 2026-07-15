#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="$ROOT_DIR/build/plugins/katecodexpanel.so"
DEST="/usr/lib/x86_64-linux-gnu/qt6/plugins/kf6/ktexteditor/katecodexpanel.so"

if [[ ! -f "$SOURCE" ]]; then
    echo "Missing build artifact: $SOURCE" >&2
    echo "Run: cmake -B build && cmake --build build" >&2
    exit 1
fi

install -Dm755 "$SOURCE" "$DEST"
echo "Installed to $DEST"
