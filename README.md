# stadiacast-esp32

Standalone Seeed XIAO ESP32-S3 Wi-Fi dongle that emulates Google's Stadia cloud
endpoints (SoftAP, DNS sink, SNTP, HTTPS/GMsg, dual DTLS 1.2 + SRTP sessions on
UDP 50000 & 50002, and Core-1 full-duplex 48 kHz Opus audio) and exposes the
Stadia Controller over USB-C as a native composite **Google Stadia Controller
(`18d1:9400`)**:

- **1000 Hz USB HID Gamepad**: full 17-button + dual-stick + analog trigger support
- **Rumble Feedback**: `hid-google-stadiaff` / HID Report 5 dual-motor vibration
- **Bidirectional USB Audio (UAC1)**:
  - **Headset Output (Speaker)**: 48 kHz, 16-bit Stereo PCM → Opus → SRTP → controller 3.5 mm jack
  - **Headset Input (Mic)**: controller 3.5 mm mic → SRTP → Opus → 48 kHz, 16-bit Mono PCM

## Hardware

- **Seeed Studio XIAO ESP32-S3** (8 MB Flash, 8 MB OPI PSRAM)
- **Google Stadia Controller** running original **Wi-Fi firmware** (not converted to Bluetooth)
  - SoftAP SSID: `STADIA_ESP32`
  - SoftAP Password: `stadiacast`
- Optional jumper on **D0 (GPIO1) → GND**:
  - **Grounded at power-on**: Serial / Flash mode (USB HID + Audio disabled so the USB Serial/JTAG port stays open)
  - **Open (default)**: Normal USB HID + UAC1 Audio mode

---

## Flashing Pre-Built Firmware

Download `stadiacast-esp32-v1.0.0-merged.bin` from the
[GitHub Releases](https://github.com/erikb-lab/stadiacast-esp32/releases) page.

### Step 1: Put the XIAO ESP32-S3 into Flash / Bootloader Mode

Because the firmware switches the USB-C port into HID + Audio mode on normal boot, put the board into ROM bootloader mode before flashing:

1. Hold the **`B` (BOOT)** button on the XIAO ESP32-S3.
2. While holding `B`, plug the USB-C cable into your computer (or tap **`R` (RESET)** if already plugged in).
3. Release **`B`**.

*(Alternatively, if you have a jumper shorting **D0 (GPIO1)** to **GND**, just plug the board in normally.)*

### Option A: Flash in Your Browser (Windows, macOS, Linux)

No software install required (requires Chrome, Edge, or Brave):

1. Open [Adafruit WebSerial ESPTool](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/) or [ESP Web Tool](https://esp.huhn.me/).
2. Click **Connect** and select the **USB JTAG/serial debug unit** (`COMx` / `usbmodem` / `ttyACM0`).
3. Set offset to **`0x0`**, select `stadiacast-esp32-v1.0.0-merged.bin`, and click **Program**.
4. When complete, unplug and replug the XIAO ESP32-S3 (making sure D0 is not grounded).

### Option B: Flash with `esptool` (CLI)

Install `esptool` once (requires Python 3):

```sh
pip install esptool
```

#### Windows (PowerShell / Command Prompt)

1. Open **Device Manager → Ports (COM & LPT)** and note the port for **USB Serial Device** (for example, `COM5`).
2. Run:

```powershell
python -m esptool --chip esp32s3 --port COM5 --baud 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 8MB `
  0x0 stadiacast-esp32-v1.0.0-merged.bin
```

*(If `COMx` does not appear, verify you are using a USB-C data cable, not a charge-only cable. Windows 10/11 includes the built-in `usbser.sys` driver for ESP32-S3 USB Serial/JTAG.)*

#### macOS (Terminal)

1. Find the serial port:

```sh
ls /dev/cu.usbmodem*
```

2. Flash (replace `/dev/cu.usbmodem1101` with the port printed above):

```sh
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 stadiacast-esp32-v1.0.0-merged.bin
```

#### Linux (Terminal)

1. Ensure your user is in the `dialout` (Debian/Ubuntu) or `uucp` (Arch) group, or prefix with `sudo`. Find the port:

```sh
ls /dev/ttyACM*
```

2. Flash (usually `/dev/ttyACM0`):

```sh
python3 -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 stadiacast-esp32-v1.0.0-merged.bin
```

3. Unplug and replug the XIAO ESP32-S3. Verify enumeration with:

```sh
lsusb -d 18d1:9400
aplay -l
arecord -l
```

You should see **`18d1:9400 Google Inc. Stadia Controller rev. A`** plus both a playback and capture audio device.

---

## Using the Dongle

1. Plug the flashed XIAO ESP32-S3 into your PC/Mac/Linux host (D0 open).
2. Provision your Wi-Fi Stadia Controller to the SoftAP **`STADIA_ESP32`** (password: **`stadiacast`**) if it is not already linked.
3. Turn on the Stadia Controller. Within ~4–6 seconds it will connect and turn solid white.
4. Plug a 3.5 mm headset into the controller and select **`Stadia Controller rev. A`** as your OS audio output and/or input device.

---

## Building from Source

Requires **ESP-IDF v5.5.4**:

```sh
source /path/to/esp-idf-5.5.4/export.sh
idf.py build
./tools/flash.sh
```

### Certificates

`main/certs/fullchain.pem` (public GTS certificate chain) is committed. The
matching private key `main/certs/privkey.pem` is git-ignored; copy it into
`main/certs/privkey.pem` before building from source.

## Attribution

Protocol reverse-engineering and HID descriptor:
[jkfran/stadiacast](https://github.com/jkfran/stadiacast) (MIT).
