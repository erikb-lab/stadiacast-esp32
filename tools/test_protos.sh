#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
gcc -DPB_FIELD_32BIT -DPB_ENABLE_MALLOC \
  -I components/nanopb -I components/stadia_proto \
  components/nanopb/pb_encode.c components/nanopb/pb_decode.c components/nanopb/pb_common.c \
  components/stadia_proto/*.pb.c tests/test_protos.c \
  -o tests/test_protos
./tests/test_protos
echo "=== Python cross-decode ==="
PYTHONPATH=/home/erik/esp/stadiacast/src python3 tools/cross_decode.py