# ESP32 Sonos Control Panel

A desktop Sonos controller in a 3D-printed picture-frame enclosure, built around a 3.5" ESP32 display board. Browse album art, control playback, manage room groups, and adjust per-room volume — powered by a single USB-C cable.

<img width="3843" height="2874" alt="ESP32 display" src="https://github.com/user-attachments/assets/895b4b82-e93e-4348-9414-f989306ef06d" />


## Features

- **Now Playing** — album art, track title, artist, progress bar, play/pause/skip/previous
- **Favorites** — scrollable grid of Sonos favorites with album art thumbnails; tap to play
- **Rooms** — see all Sonos speakers, group/ungroup with a tap, adjust per-room volume independently
- Swipe left/right to navigate between screens
- Volume control mode on the Now Playing screen (tap the volume icon)
- Auto-sleep display after 2 minutes of inactivity
- OTA firmware updates over WiFi — no USB cable needed after the first flash

---

## Hardware

### Required components

| Component | Notes |
|---|---|
| [3.5" ESP32 display board](https://www.amazon.com/dp/B0G3WDGSWG) | All-in-one ESP32-S3 + ILI9488 480×320 display + GT911 capacitive touch |
| USB-C cable + power supply | Powers the board |
| 3D-printed enclosure | See `3d-prints/` — picture-frame style desktop stand |

### Wiring

| Signal | ESP32-S3 GPIO |
|---|---|
| Display SCLK | 42 |
| Display MOSI | 39 |
| Display DC | 41 |
| Display CS | 40 |
| Display RST | 2 |
| Touch SDA (I2C) | 15 |
| Touch SCL (I2C) | 16 |
| Touch INT | 47 |
| Touch RST | 48 |

These are the pin assignments for the linked board. If you use a different board, update `firmware/include/LovyanGFX_Driver.h` (display SPI) and `firmware/include/touch.h` (touch I2C) accordingly.

---

## Software Architecture

```
[Sonos speakers]
       ↕  UPnP/SOAP
[node-sonos-http-api]  :5005   ← third-party Sonos REST API
       ↕  HTTP
[bridge server]        :3000   ← this repo (art resizing, zones, favorites)
       ↕  WiFi / HTTP
[ESP32-S3 firmware]            ← this repo (LVGL touchscreen UI)
```

Both the `node-sonos-http-api` and bridge server run on a Mac or Linux machine on the same network as your Sonos speakers.

---

## Prerequisites

- **macOS or Linux** machine on the same WiFi as your Sonos system (can be a Raspberry Pi, Mac Mini, etc.)
- **Node.js** v18+
- **PM2**: `npm install -g pm2`
- **PlatformIO** CLI or VS Code extension

---

## Setup

### 1. Clone this repo

```bash
git clone https://github.com/kravindrasc/ESP32-Sonos-Control-Panel.git
cd ESP32-Sonos-Control-Panel
```

### 2. Install node-sonos-http-api

Clone it as a sibling folder inside the repo root (it's gitignored):

```bash
git clone https://github.com/jishi/node-sonos-http-api.git
cd node-sonos-http-api
npm install --production
cd ..
```

### 3. Configure and start the bridge server

```bash
cd bridge
npm install
cp .env.example .env
```

Edit `bridge/.env` and set your Sonos speaker IP:

```
SONOS_PLAYER_IP=192.168.1.xxx
PORT=3000
```

To find your speaker's IP: open the Sonos app → Settings → System → About My System, or check your router's connected-devices list.

Start both services with PM2:

```bash
cd ..   # back to repo root
pm2 start ecosystem.config.cjs
pm2 save
pm2 startup   # follow the printed command to auto-start on login/boot
```

Verify everything is running:

```bash
pm2 list
curl http://localhost:3000/api/health
```

### 4. Configure the firmware

```bash
cd firmware
cp include/user_config.h.example include/user_config.h
```

Edit `firmware/include/user_config.h`:

```c
#define USER_WIFI_SSID      "YourNetworkName"
#define USER_WIFI_PASSWORD  "YourPassword"
#define USER_BRIDGE_BASE    "http://192.168.1.xxx:3000"   // IP of the machine running the bridge
#define USER_DEFAULT_ROOM   "Living Room"                  // Primary Sonos room (exact name from app)
```

### 5. Build and flash

**First flash — requires USB cable:**

```bash
cd firmware
pio run -e esp32-s3-devkitc-1 -t upload
```

**All future updates — OTA over WiFi:**

```bash
pio run -e esp32-s3-devkitc-1-ota -t upload
```

The device announces itself as `sonos-frame.local` on your network.

---

## Usage

### Navigation

Swipe left or right anywhere on the screen to move between the three screens:

```
[Home / Favorites]  ←→  [Now Playing]  ←→  [Rooms]
```

### Now Playing

- **Tap play/pause, skip, previous** buttons to control playback
- **Tap the volume icon** to enter volume mode; tap again to cycle back
- In volume mode, tap **+** / **−** to adjust volume across all grouped rooms proportionally

### Rooms

- Each row shows a room name and its current volume
- **Tap the left half of a row** to group or ungroup that room
- **Tap + / −** on the right side to adjust that room's volume independently
- Ungrouped rooms have their controls dimmed

### Home / Favorites

- Tap any favorite to start playing it immediately on all grouped rooms
- Favorites are loaded from your Sonos favorites list automatically

### Display sleep

The display sleeps after 2 minutes of inactivity. Tap anywhere to wake it.

---

## Customization

### Limits

Edit the top of `firmware/src/main.cpp`:

```c
#define MAX_ROOMS      16   // Maximum number of Sonos rooms shown
#define MAX_FAVORITES  20   // Maximum number of favorites shown
```

### Polling interval

```c
static const uint32_t POLL_MS = 2000;   // How often to refresh Now Playing (ms)
```

### Display and touch pins

- Display SPI pins: `firmware/include/LovyanGFX_Driver.h`
- Touch I2C pins: `firmware/include/touch.h`

### Bridge port

Set `PORT` in `bridge/.env` and update `USER_BRIDGE_BASE` in `firmware/include/user_config.h` to match.

---

## 3D Print Files

The [`3d-prints/`](3d-prints/) directory contains the enclosure for the display board. It's a picture-frame style desktop stand designed to hold the 3.5" ESP32 display board upright, with a cutout for the USB-C cable at the back.

The file is in `.3mf` format, compatible with PrusaSlicer, Bambu Studio, and most modern slicers.

---

## Troubleshooting

**Device won't connect to WiFi**
Check `USER_WIFI_SSID` and `USER_WIFI_PASSWORD` in `user_config.h`. The SSID is case-sensitive.

**Now Playing shows "Unknown Title"**
The bridge can't reach Sonos. Verify `SONOS_PLAYER_IP` in `bridge/.env` is correct, and that `pm2 list` shows both `sonos-api` and `sonos-bridge` as `online`.

**Room not appearing**
The room name in `USER_DEFAULT_ROOM` must match exactly what appears in the Sonos app (including capitalisation and spacing).

**OTA upload fails**
Ensure the device is on and `sonos-frame.local` resolves on your network (`ping sonos-frame.local`). If it doesn't resolve, use the device's IP address directly in `platformio.ini` under `upload_port`.

**Album art not loading**
The bridge caches art in `bridge/art-cache/`. If thumbnails look stale, delete that directory and restart the bridge (`pm2 restart sonos-bridge`).

---

## License

MIT

---

## Disclaimer

This project is not affiliated with, endorsed by, or sponsored by Sonos, Inc. Sonos is a registered trademark of Sonos, Inc. This is an independent open-source project that communicates with Sonos devices on a local network.
