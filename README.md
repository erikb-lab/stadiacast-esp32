# stadiacast-esp32

Standalone XIAO ESP32-S3 Wi-Fi dongle that emulates Google's Stadia cloud
endpoints (SoftAP, DNS sink, SNTP, HTTPS/GMsg, dual DTLS 1.2 sessions on
UDP 50000 & 50002) and exposes the controller over USB-C as a wired Google
Stadia Controller (18d1:9400) at 1000 Hz with full 17-button support and
rumble feedback (hid-google-stadiaff / Report 5).

## Hardware

- Seeed XIAO ESP32-S3 (8 MB Flash, 8 MB OPI PSRAM)
- Stadia Controller running Wi-Fi firmware (SoftAP `STADIA_ESP32`)

## Build & flash

ESP-IDF 5.5.4:

```sh
source /home/erik/esp/env-554.sh
TMPDIR=/home/erik/esp/.tmp idf.py build
./tools/flash.sh
```

Enter bootloader mode via USB HID: hold `B`, tap `R`, release `B`.

## Certificates

`main/certs/fullchain.pem` (public) is committed. The private key
`main/certs/privkey.pem` is git-ignored; if it is missing, copy it from
the reference server:

```sh
cp /home/erik/esp/stadia-dtls-spike/certs/privkey.pem main/certs/
```

## Attribution

Protocol reverse-engineering and HID descriptor:
[jkfran/stadiacast](https://github.com/jkfran/stadiacast) (MIT).
