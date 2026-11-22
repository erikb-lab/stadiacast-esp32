# stadiacast-esp32

Standalone Seeed XIAO ESP32-S3 Wi-Fi dongle that emulates Google's Stadia cloud
endpoints (SoftAP, DNS sink, SNTP, HTTPS/GMsg, dual DTLS 1.2 + SRTP sessions on
UDP 50000 & 50002, and Core-1 event-driven full-duplex 48 kHz Opus audio) and
exposes your Stadia Controller over USB-C as a **Stadia Wireless Adapter**
(composite Gamepad + Bidirectional USB Audio Headset):

- **Dual USB Gamepad Modes (1000 Hz + Rumble, switchable live from the controller)**:
  - **Stadia HID Mode (`18d1:9400`, default — `Stadia Wireless Adapter`):** Native Google Stadia Controller ID with all 17 buttons (including Assistant & Capture) and Report 5 rumble (`hid-google-stadiaff` on Linux, Steam Input, SDL2/SDL3, macOS, Android).
  - **XInput Mode (`1209:9401` — `Stadia Wireless Adapter XInput`):** Xbox 360 XInput controller (`0xFF/0x5D/0x01` + MS OS 2.0 `XUSB10`) with native plug-and-play support & rumble on Windows (`xusb22.sys`) and Linux (`xpad`).
- **Bidirectional USB Audio (UAC1) in Both Modes**:
  - **Headset Output (Speaker):** 48 kHz, 16-bit Stereo PCM → Opus → SRTP → controller 3.5 mm jack
  - **Headset Input (Mic):** controller 3.5 mm mic → SRTP → Opus → 48 kHz, 16-bit Mono PCM
  - **Hardware Mic Mute** & **Configurable Mic Gain** (`1.0x / 1.5x / 2.0x / 2.5x`, saved in NVS)
- **On-Board BLE Auto-Provisioning (Zero Phone/PC App Needed):**
  - Put any Wi-Fi Stadia Controller into orange-blink setup mode near the dongle and the ESP32-S3 automatically pairs it over Bluetooth LE (`0xFE2C` Fast Pair → `0xFEA0` RSA-2048 encrypted credentials). BLE scanning pauses completely while a controller is connected over Wi-Fi so there is zero gaming/audio jitter.
- **Web Dashboard + Over-the-Air (OTA) Firmware Updates (`http://192.168.4.1`):**
  - Dual 3 MB OTA partitions with automatic rollback protection.
  - Configure USB mode, mic mute/gain, stick deadzone, Wi-Fi TX power, custom SoftAP SSID/password, and upload firmware `.bin` files right from any browser — or save your Home Wi-Fi for 1-button GitHub/LAN Auto-OTA (**Assistant + Capture + B**).
- **Live Controller Battery Reporting**:
  - **Native OS Battery Gauge (Stadia HID mode):** Standard USB HID Battery Strength (Report ID 4, `0–100%`) — shows up automatically in Linux UPower / KDE Plasma / GNOME (`gaming-input`) with zero extra drivers or daemons. Reads initial charge immediately on connect (`GetControllerStateResponse.f10`), tracks live 1% steps (`notification10`), and caches the last known level in NVS across reboots.
  - **Low-Battery Alerts & On-Demand Check (Both Modes):** Haptic alerts at `≤ 15%` (3 buzzes) and `≤ 5%` (5 buzzes) with hysteresis, status LED double-dip warning, plus an instant quartile buzz hotkey.
- **Radial Stick Deadzone (`0% / 4% / 8%`, saved in NVS)** & **On-Board Status LED (`GPIO 21`)**

## Controller Hotkeys

| Action | Button Combo | Haptic Confirmation |
| :--- | :--- | :--- |
| **Switch to Stadia HID (`18d1:9400`)** | Hold **Assistant + Capture + A** (2 s) | 1 long buzz, reboots (~4–5 s reconnect) |
| **Switch to XInput (`1209:9401`)** | Hold **Assistant + Capture + X** (2 s) | 2 long buzzes, reboots (~4–5 s reconnect) |
| **Home Wi-Fi / LAN Auto-OTA** | Hold **Assistant + Capture + B** (2 s) | 3 buzzes, downloads latest release & reboots |
| **USB Serial / Flash Mode** | Hold **Assistant + Capture + Y** (2 s) | 4 buzzes, reboots to USB-Serial/JTAG |
| **Toggle Mic Mute** | Tap **Assistant** *(XInput)* or **Assistant + Capture** *(both modes)* | 1 short buzz = muted, 2 quick taps = unmuted |
| **Mic Gain Up / Down** | Hold **Assistant + Capture** + tap **D-Pad Up / Down** | 1–4 quick taps (`1.0x / 1.5x / 2.0x / 2.5x`) |
| **Cycle Stick Deadzone** | Hold **Assistant + Capture** + tap **D-Pad Right** | 1–3 quick taps (`0% / 4% / 8%`) |
| **Check Battery Level** | Hold **Assistant + Capture** + tap **D-Pad Left** | 1–4 quick taps (`≤25% / 26–50% / 51–75% / 76–100%`; soft tap if gauge pending) |

All settings (USB mode, mic gain, stick deadzone, and last known battery level) persist across reboots in NVS flash.

## Status LED (XIAO Orange User LED)

- **Slow heartbeat (100 ms / 1 s):** Waiting for Stadia Controller
- **Solid ON:** Controller connected & mic active
- **Double-dip OFF every 2 s:** Controller connected & **low battery (`≤ 15%`)**
- **Steady blink (500 ms):** Controller connected & **mic muted**
- **Fast blink (100 ms):** Holding a 2 s mode-switch combo

## Hardware

- **Seeed Studio XIAO ESP32-S3** (8 MB Flash, 8 MB OPI PSRAM)
- **Google Stadia Controller** running original **Wi-Fi firmware** (not converted to Bluetooth)
  - SoftAP SSID: `STADIA_ESP32`
  - SoftAP Password: `stadiacast`

---

## Flashing Pre-Built Firmware

### Fastest Way: 1-Click Browser Web Flasher (Chrome / Edge / Brave)

Open the **[Stadia Wireless Adapter Web Flasher](https://erikb-lab.github.io/stadiacast-esp32/)** (built automatically by GitHub Actions on every release):

1. Hold **`B` (BOOT)** on your XIAO ESP32-S3, plug it into USB-C (or tap **`R`**), and release **`B`**.
2. Click **Connect & Install Firmware**, pick the **USB JTAG/serial debug unit**, and wait for flashing to finish.
3. Unplug and replug the XIAO ESP32-S3.

---

### Manual Download (`esptool` CLI)

Download `stadiacast-esp32-v1.5.1-merged.bin` (initial USB flash) or `stadiacast-esp32.bin` (Wi-Fi OTA via `http://192.168.4.1`) from the
[GitHub Releases](https://github.com/erikb-lab/stadiacast-esp32/releases) page.

### Step 1: Enter Bootloader Mode

Because the firmware switches USB-C into Gamepad + Audio mode on normal boot, put the XIAO ESP32-S3 into ROM bootloader mode before flashing:

1. Hold the **`B` (BOOT)** button on the XIAO ESP32-S3.
2. While holding `B`, plug the USB-C cable into your computer (or tap **`R` (RESET)** if already plugged in).
3. Release **`B`**.

### Option A: Flash in Your Browser (Windows, macOS, Linux)

No software install required (requires Chrome, Edge, or Brave):

1. Open [Adafruit WebSerial ESPTool](https://adafruit.github.io/Adafruit_WebSerial_ESPTool/) or [ESP Web Tool](https://esp.huhn.me/).
2. Click **Connect** and select the **USB JTAG/serial debug unit** (`COMx` / `usbmodem` / `ttyACM0`).
3. Set offset to **`0x0`**, select `stadiacast-esp32-v1.5.1-merged.bin`, and click **Program**.
4. When complete, tap **`R` (RESET)** or unplug and replug the XIAO ESP32-S3.

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
  0x0 stadiacast-esp32-v1.5.1-merged.bin
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
  0x0 stadiacast-esp32-v1.5.1-merged.bin
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
  0x0 stadiacast-esp32-v1.5.1-merged.bin
```

3. Unplug and replug the XIAO ESP32-S3. Verify enumeration with:

```sh
lsusb | grep -E "18d1:9400|1209:9401"
aplay -l
arecord -l
```

You should see **`18d1:9400`** (**`Stadia Wireless Adapter`**) or **`1209:9401`** (**`Stadia Wireless Adapter XInput`**) plus both a playback and capture audio device.

---

## Using the Dongle

1. Plug the flashed XIAO ESP32-S3 into your Windows, macOS, or Linux host.
2. Provision your Wi-Fi Stadia Controller to the SoftAP **`STADIA_ESP32`** (password: **`stadiacast`**) if it is not already linked.
3. Turn on the Stadia Controller. Within ~4–6 seconds it will connect and turn solid white (the XIAO orange LED turns solid).
4. Plug a 3.5 mm headset into the controller and select **`Stadia Wireless Adapter`** as your OS audio output and/or input device.

---

## Building from Source

Requires **ESP-IDF v5.5.4**:

```sh
source /path/to/esp-idf-5.5.4/export.sh
idf.py build
./tools/flash.sh
```

Put the board in bootloader mode before running `./tools/flash.sh`: hold **`B`**, tap **`R`**, release **`B`**.

### Certificates

`main/certs/fullchain.pem` (public GTS certificate chain) is committed. The
matching private key `main/certs/privkey.pem` is git-ignored; copy it into
`main/certs/privkey.pem` before building from source.

## Attribution

Protocol reverse-engineering and HID descriptor:
[jkfran/stadiacast](https://github.com/jkfran/stadiacast) (MIT).
