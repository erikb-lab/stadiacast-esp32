#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

export BLOB_DIR="${BLOB_DIR:-$REPO_DIR/.tmp}"
mkdir -p "$BLOB_DIR"

gcc -DPB_FIELD_32BIT -DPB_ENABLE_MALLOC \
  -I components/nanopb -I components/stadia_proto \
  components/nanopb/pb_encode.c components/nanopb/pb_decode.c components/nanopb/pb_common.c \
  components/stadia_proto/*.pb.c tests/test_protos.c \
  -o tests/test_protos
./tests/test_protos

STADIACAST_SRC="${STADIACAST_SRC:-$REPO_DIR/../stadiacast/src}"
if [[ -d "$STADIACAST_SRC" ]] && python3 -c "import google.protobuf" 2>/dev/null; then
  echo "=== Python cross-decode ==="
  STADIACAST_SRC="$STADIACAST_SRC" PYTHONPATH="$STADIACAST_SRC" python3 tools/cross_decode.py
fi
