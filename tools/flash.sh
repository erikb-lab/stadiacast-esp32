#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:-/dev/ttyACM0}"
cd "$REPO_DIR"

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        # shellcheck source=/dev/null
        source "$IDF_PATH/export.sh" || true
    elif [[ -f "$HOME/esp/esp-idf-5.5.4/export.sh" ]]; then
        # shellcheck source=/dev/null
        source "$HOME/esp/esp-idf-5.5.4/export.sh" || true
    elif [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
        # shellcheck source=/dev/null
        source "$HOME/esp/esp-idf/export.sh" || true
    fi
fi

idf.py build

if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL=(esptool.py)
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL=(esptool)
else
    ESPTOOL=(python3 -m esptool)
fi

"${ESPTOOL[@]}" \
  --chip esp32s3 --port "$PORT" -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0xf000 build/ota_data_initial.bin \
  0x20000 build/stadiacast-esp32.bin

echo "Flashed $PORT. Unplug and replug (or tap R) to boot into Stadia Wireless Adapter mode."
