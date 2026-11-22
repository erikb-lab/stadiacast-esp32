#!/usr/bin/env bash
# Generate nanopb .pb.c / .pb.h from Stadiacast's 6 proto schemas.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PROTO_DIR="$REPO_DIR/proto"
OUT_DIR="$REPO_DIR/components/stadia_proto"
PROTOC="${PROTOC:-$(command -v protoc)}"
PLUGIN="${NANOPB_PLUGIN:-$(command -v protoc-gen-nanopb)}"
NANOPB_DIR="${NANOPB_DIR:-$REPO_DIR/components/nanopb}"

mkdir -p "$OUT_DIR"

PROTOS="discovery_api.proto webrtc_sdp.proto rpc_message.proto controller_message.proto streamer_message.proto input.proto"

for p in $PROTOS; do
  echo "Generating $p ..."
  "$PROTOC" \
    --plugin=protoc-gen-nanopb="$PLUGIN" \
    --nanopb_out="$OUT_DIR" --nanopb_opt="--options-path=$PROTO_DIR" \
    -I"$PROTO_DIR" -I"$NANOPB_DIR" \
    "$PROTO_DIR/$p"
done

echo "Generated files:"
ls "$OUT_DIR"
