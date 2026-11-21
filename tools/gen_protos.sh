#!/usr/bin/env bash
# Generate nanopb .pb.c / .pb.h from Stadiacast's 6 proto schemas.
set -euo pipefail

PROTO_DIR="/home/erik/esp/stadiacast-esp32/proto"
OUT_DIR="/home/erik/esp/stadiacast-esp32/components/stadia_proto"
PROTOC="/home/erik/.local/bin/protoc"
PLUGIN="/home/erik/esp/.venv-proto/bin/protoc-gen-nanopb"
NANOPB_DIR="/home/erik/esp/.tmp/nanopb-src"

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