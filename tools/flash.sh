#!/usr/bin/env bash
set -euo pipefail
TMPDIR=/home/erik/esp/.tmp
export TMPDIR
export ESP_ROM_ELF_DIR=/home/erik/.espressif/tools/esp-rom-elfs/20241011
source /home/erik/esp/env-554.sh || true   # export.sh returns 1 under set -e in this env; continue anyway
cd "$(dirname "$0")/.."
idf.py build
~/.arduino15/packages/esp32/tools/esptool_py/5.2.0/esptool \
  --chip esp32s3 --port /dev/ttyACM0 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/stadiacast-esp32.bin
echo "Flashed. Open serial: picocom --baud 115200 --flow n --imap lfcrlf --lower-dtr --lower-rts /dev/ttyACM0"
