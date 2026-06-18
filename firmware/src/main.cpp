#include "user_config.h"
#include "pins_config.h"
#include "LovyanGFX_Driver.h"

#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <TJpg_Decoder.h>
#include <ArduinoOTA.h>

#include "touch.h"

// SF Pro Display fonts (subset generated via lv_font_conv for printable ASCII)
LV_FONT_DECLARE(sf_pro_26);

// Material Icons fonts (outlined, generated from material-icons-outlined.woff via lv_font_conv)
LV_FONT_DECLARE(lv_font_material_26);  // music_note — fav row placeholders
LV_FONT_DECLARE(lv_font_material_36);  // menu, volume_up — header
LV_FONT_DECLARE(lv_font_material_38);  // shuffle, repeat, repeat_one — playmode
LV_FONT_DECLARE(lv_font_material_48);  // skip_prev, play_arrow, skip_next — transport
LV_FONT_DECLARE(lv_font_material_82);  // music_note — art placeholder

// UTF-8 encoded Material Icons codepoints
#define MI_PAUSE       "\xEE\x80\xB4"   // U+E034 pause
#define MI_PLAY        "\xEE\x80\xB7"   // U+E037 play_arrow    (48px font)
#define MI_SKIP_NEXT   "\xEE\x81\x84"   // U+E044 skip_next     (48px font)
#define MI_SKIP_PREV   "\xEE\x81\x85"   // U+E045 skip_previous (48px font)
#define MI_VOL_MINUS   "\xEE\x85\x9D"   // U+E15D remove_circle_outline (48px, vol –)
#define MI_VOL_PLUS    "\xEE\x85\x88"   // U+E148 add_circle_outline    (48px, vol +)
#define MI_MENU        "\xEE\x97\x92"   // U+E5D2 menu          (36px, header)
#define MI_VOLUME_UP   "\xEE\x81\x90"   // U+E050 volume_up     (36px, header)
#define MI_MUSIC_NOTE  "\xEE\x90\x85"   // U+E405 music_note    (82px, placeholder)
#define MI_TV          "\xEE\x8C\xB3"   // U+E333 tv            (38px playmode toggle, 82px art placeholder)
#define MI_SPEAKER     "\xEE\x8C\xAD"   // U+E32D speaker       (36px, room picker header)
#define MI_CHECK_CIR   "\xEE\xA1\xAC"   // U+E86C check_circle  (36px, room selected)
#define MI_RADIO_OFF   "\xEE\xA0\xB6"   // U+E836 radio_button_unchecked (36px, room unselected)
#define MI_SHUFFLE     "\xEE\x81\x83"   // U+E043 shuffle       (36px, playmode)
#define MI_REPEAT      "\xEE\x81\x80"   // U+E040 repeat        (36px, playmode)
#define MI_REPEAT_ONE  "\xEE\x81\x81"   // U+E041 repeat_one    (36px, playmode)

static const char* WIFI_SSID     = USER_WIFI_SSID;
static const char* WIFI_PASSWORD = USER_WIFI_PASSWORD;
static const char* BRIDGE_BASE   = USER_BRIDGE_BASE;
static const char* SONOS_ROOM    = USER_DEFAULT_ROOM;
static char        gActiveRoom[64] = USER_DEFAULT_ROOM;
static const uint32_t POLL_MS    = 2000;

// --- LVGL display buffers ---
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf_a[LCD_H_RES * 40];
static lv_color_t buf_b[LCD_H_RES * 40];

uint16_t touchX, touchY;

// --- Sonos state ---
struct SonosState {
  String room, title, artist, album, artUrl, sourceType, playbackState;
  bool   mute        = false;
  bool   shuffle     = false;
  String repeat      = "none";  // "none", "all", "one"
  int    volume      = 0;
  int    elapsedSec  = 0;
  int    durationSec = 0;
};
static SonosState    gState;
static unsigned long lastPollMs = 0;
static bool          wifiOk     = false;

// --- Display sleep ---
#define BL_PIN           38
#define SLEEP_TIMEOUT_MS (5UL * 60UL * 1000UL)
static bool          displayOn     = true;
static unsigned long lastActivityMs = 0;

static void set_display(bool on) {
  if (displayOn == on) return;
  displayOn = on;
  digitalWrite(BL_PIN, on ? HIGH : LOW);
  if (on) lastPollMs = 0;  // force immediate now-playing refresh on wake
}

static void poke_activity() {
  lastActivityMs = millis();
  if (!displayOn) set_display(true);
}

// --- Album art canvas (PSRAM-backed) ---
#define ART_SIZE       240
#define FAV_THUMB_SIZE  64
static lv_color_t* artCanvasBuf   = nullptr;
static lv_obj_t*   artClipBox     = nullptr;   // clip container — enforces rounded corners on canvas
static lv_obj_t*   artCanvas      = nullptr;
static lv_obj_t*   artPlaceholder = nullptr;
// Cache key: track title — the bridge art URL is always the same path regardless of song
static String      lastArtTrack  = "";

// TJpg callback: write decoded tiles into artCanvasBuf
static lv_color_t* gArtDst = nullptr;
static bool jpeg_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!gArtDst) return false;
  for (int row = 0; row < h; row++) {
    int dy = y + row;
    if (dy >= ART_SIZE) break;
    for (int col = 0; col < w; col++) {
      int dx = x + col;
      if (dx >= ART_SIZE) break;
      gArtDst[dy * ART_SIZE + dx].full = bitmap[row * w + col];
    }
  }
  return true;
}

// TJpg callback for FAV_THUMB_SIZE×FAV_THUMB_SIZE fav thumbnails
static lv_color_t* gFavArtDst = nullptr;
static bool fav_jpeg_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (!gFavArtDst) return false;
  for (int row = 0; row < h; row++) {
    int dy = y + row;
    if (dy >= FAV_THUMB_SIZE) break;
    for (int col = 0; col < w; col++) {
      int dx = x + col;
      if (dx >= FAV_THUMB_SIZE) break;
      gFavArtDst[dy * FAV_THUMB_SIZE + dx].full = bitmap[row * w + col];
    }
  }
  return true;
}

// --- Screenshot support ---
static uint16_t* gFrameBuffer     = nullptr;
static bool      gScreenshotReady = false;
static WiFiServer screenshotServer(8080);

static bool init_screenshot_buffer() {
  if (gFrameBuffer) return true;
  size_t pixels = (size_t)LCD_H_RES * (size_t)LCD_V_RES;
  gFrameBuffer = (uint16_t*)ps_malloc(pixels * sizeof(uint16_t));
  if (!gFrameBuffer) { Serial.println("ERROR: screenshot framebuffer alloc failed"); return false; }
  memset(gFrameBuffer, 0, pixels * sizeof(uint16_t));
  return true;
}

static void copy_area_to_framebuffer(const lv_area_t* area, const lv_color_t* color_p) {
  // Skip during screen transitions — screenshot data isn't needed mid-animation
  // and skipping frees CPU for rendering more frames.
  if (!gFrameBuffer || lv_anim_count_running() > 0) return;
  int width  = area->x2 - area->x1 + 1;
  int height = area->y2 - area->y1 + 1;
  for (int row = 0; row < height; row++) {
    int dstY = area->y1 + row;
    if (dstY < 0 || dstY >= LCD_V_RES) continue;
    memcpy(&gFrameBuffer[(size_t)dstY * LCD_H_RES + area->x1],
           color_p + row * width,
           width * sizeof(uint16_t));
  }
  gScreenshotReady = true;
}

static void serial_write_u16(uint16_t v) { uint8_t b[2]; b[0]=v&0xFF; b[1]=(v>>8)&0xFF; Serial.write(b,2); }
static void serial_write_u32(uint32_t v) { uint8_t b[4]; b[0]=v&0xFF; b[1]=(v>>8)&0xFF; b[2]=(v>>16)&0xFF; b[3]=(v>>24)&0xFF; Serial.write(b,4); }

static void emit_bmp_to_serial() {
  if (!gFrameBuffer || !gScreenshotReady) { Serial.println("ERROR: no framebuffer available"); return; }
  const int width = LCD_H_RES, height = LCD_V_RES;
  const uint32_t headerSize = 14 + 40 + 12;
  const uint32_t pixelBytes = (uint32_t)width * (uint32_t)height * 2;
  const uint32_t fileSize   = headerSize + pixelBytes;
  Serial.println("BMP_BEGIN");
  Serial.write('B'); Serial.write('M');
  serial_write_u32(fileSize); serial_write_u16(0); serial_write_u16(0); serial_write_u32(headerSize);
  serial_write_u32(40); serial_write_u32(width); serial_write_u32(height);
  serial_write_u16(1); serial_write_u16(16); serial_write_u32(3); serial_write_u32(pixelBytes);
  serial_write_u32(2835); serial_write_u32(2835); serial_write_u32(0); serial_write_u32(0);
  serial_write_u32(0xF800); serial_write_u32(0x07E0); serial_write_u32(0x001F);
  for (int y = height - 1; y >= 0; y--)
    Serial.write((const uint8_t*)(gFrameBuffer + (size_t)y * width), width * 2);
  Serial.println(); Serial.println("BMP_END");
}

static void handle_serial_commands() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 's' || c == 'S') { Serial.println("Capturing screenshot..."); emit_bmp_to_serial(); }
  }
}

// Serve BMP screenshot over HTTP on port 8080 (GET /screenshot)
static void handle_screenshot_server() {
  WiFiClient client = screenshotServer.available();
  if (!client) return;
  unsigned long t0 = millis();
  while (!client.available() && millis() - t0 < 500) delay(1);
  while (client.available()) client.read(); // drain request

  if (!gFrameBuffer || !gScreenshotReady) {
    client.print("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 15\r\nConnection: close\r\n\r\nNo framebuffer.");
    client.stop(); return;
  }

  const uint32_t W = LCD_H_RES, H = LCD_V_RES;
  const uint32_t hdrSz  = 14 + 40 + 12;
  const uint32_t pixSz  = W * H * 2;
  const uint32_t fileSz = hdrSz + pixSz;

  // HTTP headers
  client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", fileSz);

  // BMP file header (14 bytes)
  uint8_t fh[14] = {'B','M',
    uint8_t(fileSz),uint8_t(fileSz>>8),uint8_t(fileSz>>16),uint8_t(fileSz>>24),
    0,0,0,0,
    uint8_t(hdrSz),uint8_t(hdrSz>>8),0,0};
  client.write(fh, 14);

  // DIB header (40 bytes)
  uint8_t dh[40] = {40,0,0,0,
    uint8_t(W),uint8_t(W>>8),0,0,
    uint8_t(H),uint8_t(H>>8),0,0,
    1,0, 16,0, 3,0,0,0,
    uint8_t(pixSz),uint8_t(pixSz>>8),uint8_t(pixSz>>16),uint8_t(pixSz>>24),
    0x13,0x0B,0,0, 0x13,0x0B,0,0, 0,0,0,0, 0,0,0,0};
  client.write(dh, 40);

  // RGB565 color masks (12 bytes)
  uint8_t masks[12] = {0x00,0xF8,0,0, 0xE0,0x07,0,0, 0x1F,0x00,0,0};
  client.write(masks, 12);

  // Pixel data — BMP is bottom-up
  for (int y = (int)H - 1; y >= 0; y--)
    client.write((const uint8_t*)(gFrameBuffer + (size_t)y * W), W * 2);

  client.stop();
}

// --- LVGL driver callbacks ---
void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  if (gfx.getStartCount() > 0) gfx.endWrite();
  copy_area_to_framebuffer(area, color_p);
  gfx.pushImageDMA(area->x1, area->y1,
                   area->x2 - area->x1 + 1, area->y2 - area->y1 + 1,
                   (lgfx::rgb565_t*)&color_p->full);
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data) {
  (void)indev_driver;
  data->state = LV_INDEV_STATE_REL;
  if (gfx.getTouch(&touchX, &touchY)) {
    if (!displayOn) {
      poke_activity();   // wake display, swallow touch so no button fires
      return;
    }
    poke_activity();
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = touchX;
    data->point.y = touchY;
  }
}

// --- Helpers ---
static String url_encode(const char* s) {
  String out; char c;
  while ((c = *s++)) {
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out += c;
    else if (c == ' ') out += "%20";
    else { char hex[4]; snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c); out += hex; }
  }
  return out;
}

static String format_time(int totalSec) {
  if (totalSec < 0) totalSec = 0;
  char buf[16]; snprintf(buf, sizeof(buf), "%d:%02d", totalSec/60, totalSec%60);
  return String(buf);
}

static int parse_duration_to_sec(JsonVariant v) {
  if (v.is<int>()) return v.as<int>();
  const char* s = v | "";
  if (!s || !*s) return 0;
  int a=0, b=0, c=0;
  if (sscanf(s, "%d:%d:%d", &a, &b, &c) == 3) return a*3600 + b*60 + c;
  if (sscanf(s, "%d:%d",    &a, &b)    == 2) return a*60 + b;
  return atoi(s);
}

static void sanitize_state() {
  gState.title.trim();
  if (gState.title.length() == 0 || gState.title == "Unknown Title") gState.title = "Nothing Playing";
  gState.artist.trim();
  if (gState.artist == "Unknown Artist") gState.artist = "";
  String src = gState.sourceType; src.trim(); src.toLowerCase();
  if (src == "line_in") {
    if (gState.title == "Nothing Playing") gState.title = "TV input";
    if (gState.artist.length() == 0) gState.artist = "HDMI";
  }
  if (gState.durationSec < 0) gState.durationSec = 0;
  if (gState.elapsedSec  < 0) gState.elapsedSec  = 0;
  if (gState.durationSec == 0) gState.elapsedSec = 0;
  if (gState.durationSec > 0 && gState.elapsedSec > gState.durationSec)
    gState.elapsedSec = gState.durationSec;
}

// --- Favorites / home screen ---
#define MAX_FAVORITES  20
struct FavItem {
  char        title[80];
  char        artUrl[300];
  int         index;
  lv_color_t* artBuf;       // PSRAM, FAV_THUMB_SIZE² — persists across rebuilds
  lv_obj_t*   canvas;
  lv_obj_t*   placeholder;
  lv_obj_t*   card;
  bool        artLoaded;    // artBuf has valid decoded pixels for current artUrl
};
static FavItem    gFavs[MAX_FAVORITES];
static int        gFavCount     = 0;
static bool       gFavsLoaded   = false;
static lv_obj_t*  screenHome    = nullptr;
static lv_obj_t*  favGrid       = nullptr;
static lv_obj_t*  screenBoot    = nullptr;
static lv_obj_t*  bootStatusLbl = nullptr;

// --- Room picker ---
#define MAX_ROOMS 16
struct RoomItem {
  char      name[64];
  char      coordinator[64]; // Sonos zone coordinator for this room
  lv_obj_t* indicator;       // check_circle or radio_button_unchecked label
  lv_obj_t* nameLabel;
  lv_obj_t* volLabel;        // per-room volume number on rooms screen
  lv_obj_t* volMinus;        // − button (kept for dimming when deselected)
  lv_obj_t* volPlus;         // + button (kept for dimming when deselected)
  int       volume;          // last-known volume for this room
};
static RoomItem   gRooms[MAX_ROOMS];
static bool       gRoomSelected[MAX_ROOMS] = {};
static int        gRoomCount      = 0;
static bool       gRoomsLoaded    = false;
static bool       gRoomVolsFetched = false; // true once fetch_room_volumes() has succeeded
static lv_obj_t*  screenRooms   = nullptr;
static lv_obj_t*  roomGrid      = nullptr;

// --- UI widgets ---
static lv_obj_t* screenMain      = nullptr;
static lv_obj_t* labelSongTitle  = nullptr;
static lv_obj_t* labelArtist     = nullptr;
static lv_obj_t* btnPrev         = nullptr;
static lv_obj_t* btnPlayPause    = nullptr;
static lv_obj_t* btnNext         = nullptr;
static lv_obj_t* labelPlayPause  = nullptr;
static lv_obj_t* progressSlider  = nullptr;
// Volume drawers (one per screen, same layout, slide in from right)
static lv_obj_t* volumeDrawerHome = nullptr;
static lv_obj_t* labelVolHome     = nullptr;
// In-place volume controls (replaces transport row on player screen)
static lv_obj_t* btnVolDown      = nullptr;
static lv_obj_t* btnVolUp        = nullptr;
static lv_obj_t* labelVolDisplay = nullptr;
// Tap mode: 0=transport (default), 1=volume, 2=shuffle/repeat
// Cycles on art/title/artist tap; auto-reverts to 0 after 4 s of inactivity.
static int           gTapMode      = 0;
static unsigned long gTapModeMs    = 0;
static lv_obj_t* btnShuffle      = nullptr;
static lv_obj_t* btnRepeat       = nullptr;
static lv_obj_t* btnTV           = nullptr;
static lv_obj_t* lblShuffleIcon  = nullptr;
static lv_obj_t* lblRepeatIcon   = nullptr;
static lv_obj_t* lblTVIcon       = nullptr;
static bool          gMuted         = false;
static int           gFavLoadStep   = 0;   // 0=idle 1=fetch+build list
static int           gPendingFavIdx  = -1;
static int           gPendingMute    = -1;  // -1=none 0=unmute 1=mute
static bool          gPendingLineIn  = false;
static int           gFastPollCount = 0;   // rapid re-polls remaining after user action
static int16_t       gSwipeStartX   = -1;  // loop-based swipe tracking
static bool          gSwipeActive   = false;
static bool          gSwipeConsumed = false; // suppresses click event after a swipe
#define FAST_POLL_MS     500               // interval during fast-poll window
#define FAST_POLL_END_SEC 8               // seconds before track end to trigger fast poll

// --- Art display ---
static void show_placeholder_art() {
  if (!artPlaceholder) return;
  lv_obj_clean(artPlaceholder);
  // Figma token: --state-layers/on-secondary/opacity-16 = rgba(255,255,255,0.16) on black
  lv_obj_set_style_bg_color(artPlaceholder, lv_color_hex(0x292929), 0);
  lv_obj_set_style_bg_opa(artPlaceholder, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(artPlaceholder, 12, 0);
  lv_obj_set_style_border_width(artPlaceholder, 0, 0);
  lv_obj_set_style_shadow_width(artPlaceholder, 0, 0);
  lv_obj_set_style_pad_all(artPlaceholder, 0, 0);

  bool isTV = (gState.sourceType == "line_in");
  lv_obj_t* icon = lv_label_create(artPlaceholder);
  lv_label_set_text(icon, isTV ? MI_TV : MI_MUSIC_NOTE);
  lv_obj_set_style_text_font(icon, &lv_font_material_82, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(0x9E9E9E), 0);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);

  lv_obj_clear_flag(artPlaceholder, LV_OBJ_FLAG_HIDDEN);
  if (artCanvas) lv_obj_add_flag(artCanvas, LV_OBJ_FLAG_HIDDEN);
}

static void fetch_and_show_art(const String& url) {
  if (url.length() == 0) { show_placeholder_art(); lastArtTrack = ""; return; }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); show_placeholder_art(); return; }

  const size_t maxBuf = 80000;
  uint8_t* jpegBuf = (uint8_t*)ps_malloc(maxBuf);
  if (!jpegBuf) { http.end(); return; }

  WiFiClient* stream = http.getStreamPtr();
  size_t totalRead = 0;
  unsigned long deadline = millis() + 8000;
  while (millis() < deadline && totalRead < maxBuf) {
    int avail = stream->available();
    if (avail > 0) {
      size_t toRead = min((size_t)avail, maxBuf - totalRead);
      totalRead += stream->readBytes(jpegBuf + totalRead, toRead);
    } else if (!http.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  http.end();

  if (totalRead == 0) { free(jpegBuf); show_placeholder_art(); return; }

  // Clear canvas buffer to dark gray before decode
  uint16_t fillColor = lv_color_hex(0x1C1C1C).full;
  for (size_t i = 0; i < (size_t)ART_SIZE * ART_SIZE; i++) artCanvasBuf[i].full = fillColor;

  gArtDst = artCanvasBuf;
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(jpeg_cb);
  JRESULT result = TJpgDec.drawJpg(0, 0, jpegBuf, (uint32_t)totalRead);
  free(jpegBuf);

  if (result != JDR_OK) {
    Serial.printf("JPEG decode error: %d\n", (int)result);
    show_placeholder_art();
    return;
  }

  lv_obj_add_flag(artPlaceholder, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(artCanvas, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(artCanvas);
}

// --- Sonos network calls ---
static bool fetch_now_playing() {
  if (WiFi.status() != WL_CONNECTED) { wifiOk = false; return false; }

  HTTPClient http;
  String url = String(BRIDGE_BASE) + "/api/sonos/now-playing?room=" + url_encode(gActiveRoom);
  http.begin(url); http.setTimeout(5000);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) { http.end(); return false; }

  String payload = http.getString(); http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  gState.room        = doc["room"] | gActiveRoom;
  gState.title       = doc["title"] | "Nothing Playing";
  gState.artist      = doc["artist"] | "";
  gState.album       = doc["album"] | "";
  gState.artUrl      = doc["artUrl"] | "";
  gState.volume      = doc["volume"] | 0;
  gState.elapsedSec  = doc["elapsedSec"] | 0;
  gState.durationSec = parse_duration_to_sec(doc["durationSec"]);

  JsonVariant raw = doc["raw"];
  gState.mute          = raw["mute"] | false;
  gState.sourceType    = raw["currentTrack"]["type"] | "";
  gState.playbackState = raw["playbackState"] | "";
  gState.shuffle       = raw["playMode"]["shuffle"] | false;
  gState.repeat        = String(raw["playMode"]["repeat"] | "none");

  sanitize_state();
  return true;
}

static void post_room_join(const char* room, const char* coordinator) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/command");
  http.addHeader("Content-Type", "application/json"); http.setTimeout(5000);
  JsonDocument req;
  req["room"] = room; req["action"] = "join"; req["coordinator"] = coordinator;
  String body; serializeJson(req, body);
  http.POST(body); http.end();
}

static void post_room_leave(const char* room) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/command");
  http.addHeader("Content-Type", "application/json"); http.setTimeout(5000);
  JsonDocument req;
  req["room"] = room; req["action"] = "leave";
  String body; serializeJson(req, body);
  http.POST(body); http.end();
}

static bool post_sonos_command_to(const char* room, const char* action, int value, bool hasValue) {
  if (WiFi.status() != WL_CONNECTED) { wifiOk = false; return false; }
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/command");
  http.addHeader("Content-Type", "application/json"); http.setTimeout(5000);
  JsonDocument req;
  req["room"] = room; req["action"] = action;
  if (hasValue) req["value"] = value;
  String body; serializeJson(req, body);
  int code = http.POST(body); http.end();
  return (code >= 200 && code < 300);
}

static bool post_sonos_command(const char* action, int value = 0, bool hasValue = false) {
  // Send to every selected room: play/pause/skip route to coordinator via Sonos grouping,
  // while volume/mute must hit each speaker individually since groups don't share volume.
  bool ok = false;
  for (int i = 0; i < gRoomCount; i++) {
    if (gRoomSelected[i]) ok = post_sonos_command_to(gRooms[i].name, action, value, hasValue) || ok;
  }
  if (!ok) ok = post_sonos_command_to(gActiveRoom, action, value, hasValue);
  return ok;
}

static void set_tap_mode(int mode);
static void update_playmode_icons();
static void update_vol_labels();
static void nav_to_home();
static void nav_to_player();
static void nav_to_rooms();
static void apply_press_style(lv_obj_t* btn);
static void adjust_all_room_volumes(int delta);

// --- UI update ---
static void set_play_pause_icon() {
  if (!labelPlayPause) return;
  String state = gState.playbackState; state.toUpperCase();
  // Both MI_PAUSE and MI_PLAY are in the 48px outlined font (U+E034 pause, U+E037 play_arrow)
  lv_label_set_text(labelPlayPause, state == "PLAYING" ? MI_PAUSE : MI_PLAY);
}

static void update_ui() {
  sanitize_state();

  // Keep active room's per-room volume in sync with NP poll result
  if (gRoomVolsFetched) {
    for (int i = 0; i < gRoomCount; i++) {
      if (strcmp(gRooms[i].name, gActiveRoom) == 0) {
        gRooms[i].volume = gState.volume;
        break;
      }
    }
  }

  if (labelSongTitle) lv_label_set_text(labelSongTitle, gState.title.c_str());

  if (labelArtist) {
    if (gState.artist.length() == 0) lv_obj_add_flag(labelArtist, LV_OBJ_FLAG_HIDDEN);
    else { lv_obj_clear_flag(labelArtist, LV_OBJ_FLAG_HIDDEN); lv_label_set_text(labelArtist, gState.artist.c_str()); }
  }

  int sliderVal = (gState.durationSec > 0) ? (gState.elapsedSec * 100 / gState.durationSec) : 0;
  if (progressSlider)  lv_slider_set_value(progressSlider, sliderVal, LV_ANIM_OFF);

  gMuted = gState.mute;
  update_vol_labels();

  set_play_pause_icon();
  update_playmode_icons();

  // Re-fetch art when the track changes (artUrl is the same path for every song)
  if (gState.title != lastArtTrack) {
    lastArtTrack = gState.title;
    fetch_and_show_art(gState.artUrl);
    poke_activity();   // new song or source — wake display
  }
}

// --- Button event callbacks ---
static void event_prev(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (post_sonos_command("previous")) { delay(120); if (fetch_now_playing()) update_ui(); gFastPollCount = 5; lastPollMs = millis(); }
}
static void event_play_pause(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (post_sonos_command("playpause")) { delay(120); if (fetch_now_playing()) update_ui(); gFastPollCount = 3; lastPollMs = millis(); }
}
static void event_next(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (post_sonos_command("next")) { delay(120); if (fetch_now_playing()) update_ui(); gFastPollCount = 5; lastPollMs = millis(); }
}
static void update_vol_labels() {
  // When per-room volumes are known, show the average of selected rooms.
  int displayVol = gState.volume;
  if (gRoomVolsFetched && gRoomCount > 0) {
    int selSum = 0, selCount = 0;
    for (int i = 0; i < gRoomCount; i++) {
      if (gRoomSelected[i]) { selSum += gRooms[i].volume; selCount++; }
    }
    if (selCount > 0) displayVol = (selSum + selCount / 2) / selCount;
  }
  if (labelVolDisplay) {
    lv_label_set_text_fmt(labelVolDisplay, "%d", displayVol);
    lv_obj_set_style_text_color(labelVolDisplay,
      gMuted ? lv_color_hex(0x555555) : lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_decor(labelVolDisplay,
      gMuted ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
  }
  if (labelVolHome) lv_label_set_text_fmt(labelVolHome, "%d", displayVol);
}

static void set_tap_mode(int mode) {
  gTapMode = mode;
  if (mode != 0) gTapModeMs = millis();
  if (mode == 0) gMuted = false;
  lv_obj_t* transport[] = { btnPrev, btnPlayPause, btnNext };
  lv_obj_t* volctrls[]  = { btnVolDown, labelVolDisplay, btnVolUp };
  lv_obj_t* playmode[]  = { btnShuffle, btnRepeat, btnTV };
  bool showTrans = (mode == 0), showVol = (mode == 1), showPlay = (mode == 2);
  for (auto* o : transport) { if (o) { showTrans ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } }
  for (auto* o : volctrls)  { if (o) { showVol   ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } }
  for (auto* o : playmode)  { if (o) { showPlay  ? lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); } }
}

static void update_playmode_icons() {
  if (lblShuffleIcon) {
    lv_obj_set_style_text_color(lblShuffleIcon,
      gState.shuffle ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x505050), 0);
  }
  if (lblRepeatIcon) {
    lv_label_set_text(lblRepeatIcon, gState.repeat == "one" ? MI_REPEAT_ONE : MI_REPEAT);
    lv_obj_set_style_text_color(lblRepeatIcon,
      gState.repeat != "none" ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x505050), 0);
  }
  if (lblTVIcon) {
    bool tvActive = (gState.sourceType == "line_in");
    lv_obj_set_style_text_color(lblTVIcon,
      tvActive ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x505050), 0);
  }
}

static void event_playmode_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  set_tap_mode((gTapMode + 1) % 3);
}

static void event_tv_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  gTapModeMs = millis();
  if (gState.sourceType == "line_in") {
    post_sonos_command("playpause");
  } else {
    post_sonos_command("linein");
  }
  delay(200);
  if (fetch_now_playing()) { update_ui(); update_playmode_icons(); }
}

static void event_shuffle_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  gTapModeMs = millis();
  if (post_sonos_command("shuffle")) { delay(120); if (fetch_now_playing()) update_ui(); }
}

static void event_repeat_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  gTapModeMs = millis();
  if (post_sonos_command("repeat")) { delay(120); if (fetch_now_playing()) update_ui(); }
}

static void toggle_volume_drawer(lv_obj_t* drawer) {
  if (!drawer) return;
  int cur_x = lv_obj_get_x(drawer);
  bool open  = (cur_x < LCD_H_RES);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, drawer);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
  lv_anim_set_time(&a, 220);
  lv_anim_set_values(&a, cur_x, open ? LCD_H_RES : LCD_H_RES - 120);
  lv_anim_start(&a);
}

static void event_screen_clicked(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_obj_t* scr = lv_scr_act();
  lv_obj_t* drawer = volumeDrawerHome;
  if (!drawer) return;
  // Only close if drawer is open and click landed outside it (x < drawer's left edge)
  if (lv_obj_get_x(drawer) >= LCD_H_RES) return;
  lv_indev_t* indev = lv_indev_get_act();
  lv_point_t pt;
  lv_indev_get_point(indev, &pt);
  if (pt.x < lv_obj_get_x(drawer)) toggle_volume_drawer(drawer);
}

// event_volume_icon removed — volume accessed via art/title tap cycle

static void event_vol_mute(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  gMuted = !gMuted;
  gPendingMute = gMuted ? 1 : 0;
  gTapModeMs = millis();
  update_vol_labels();
}

// Proportional group volume: scale every selected room by the same ratio so
// the average moves by |delta|=1. Rooms at 0 when going up all set to newAvg.
static void adjust_all_room_volumes(int delta) {
  if (!gRoomVolsFetched || gRoomCount == 0) {
    // No per-room data — adjust primary room only.
    gState.volume = max(0, min(100, gState.volume + delta));
    update_vol_labels();
    post_sonos_command("volume", gState.volume, true);
    return;
  }
  int selSum = 0, selCount = 0;
  for (int i = 0; i < gRoomCount; i++) {
    if (gRoomSelected[i]) { selSum += gRooms[i].volume; selCount++; }
  }
  if (selCount == 0) return;
  int avg    = (selSum + selCount / 2) / selCount;
  int newAvg = max(0, min(100, avg + delta));
  if (newAvg == avg) return;

  for (int i = 0; i < gRoomCount; i++) {
    if (!gRoomSelected[i]) continue;
    int newVol = (avg == 0) ? newAvg
               : max(0, min(100, (gRooms[i].volume * newAvg + avg / 2) / avg));
    if (newVol != gRooms[i].volume) {
      gRooms[i].volume = newVol;
      post_sonos_command_to(gRooms[i].name, "volume", newVol, true);
    }
  }
  // Recalculate average for display (rounding may shift it slightly).
  selSum = 0;
  for (int i = 0; i < gRoomCount; i++) if (gRoomSelected[i]) selSum += gRooms[i].volume;
  gState.volume = (selSum + selCount / 2) / selCount;
  update_vol_labels();
}

static void event_vol_up(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gMuted) { gMuted = false; gPendingMute = 0; }
  gTapModeMs = millis();
  adjust_all_room_volumes(+1);
}

static void event_vol_down(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gMuted) { gMuted = false; gPendingMute = 0; }
  gTapModeMs = millis();
  adjust_all_room_volumes(-1);
}

// Plain clickable object — avoids lv_btn theme which cascades black text on press.
static lv_obj_t* make_clean_btn(lv_obj_t* parent) {
  lv_obj_t* btn = lv_obj_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  return btn;
}

// Creates a 120×320 volume drawer parented to `screen`, initially off-screen right.
// Returns the drawer obj. Caller stores the label pointer via out_label.
static lv_obj_t* make_volume_drawer(lv_obj_t* screen, lv_obj_t** out_label) {
  const int DW = 120;
  lv_obj_t* d = lv_obj_create(screen);
  lv_obj_set_size(d, DW, LCD_V_RES);
  lv_obj_set_pos(d, LCD_H_RES, 0);   // off-screen; slides to x=360 when opened
  lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(d, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(d, 0, 0);
  lv_obj_set_style_shadow_width(d, 0, 0);
  lv_obj_set_style_pad_all(d, 0, 0);
  lv_obj_set_style_radius(d, 0, 0);

  // "+" button
  lv_obj_t* btnUp = make_clean_btn(d);
  lv_obj_set_size(btnUp, 80, 80);
  lv_obj_align(btnUp, LV_ALIGN_TOP_MID, 0, 36);
  lv_obj_set_style_radius(btnUp, 16, 0);
  lv_obj_set_style_bg_color(btnUp, lv_color_hex(0x2C2C2E), 0);
  lv_obj_set_style_bg_opa(btnUp, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btnUp, lv_color_hex(0x48484A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btnUp, 0, 0);
  lv_obj_set_style_shadow_width(btnUp, 0, 0);
  lv_obj_add_event_cb(btnUp, event_vol_up, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblUp = lv_label_create(btnUp);
  lv_label_set_text(lblUp, "+");
  lv_obj_set_style_text_font(lblUp, &sf_pro_48, 0);
  lv_obj_set_style_text_color(lblUp, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(lblUp, LV_ALIGN_CENTER, 0, -4);

  // Volume number
  lv_obj_t* volNum = lv_label_create(d);
  lv_label_set_text(volNum, "0");
  lv_obj_set_style_text_font(volNum, &sf_pro_48, 0);
  lv_obj_set_style_text_color(volNum, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(volNum, LV_ALIGN_CENTER, 0, 0);
  if (out_label) *out_label = volNum;

  // "-" button
  lv_obj_t* btnDn = make_clean_btn(d);
  lv_obj_set_size(btnDn, 80, 80);
  lv_obj_align(btnDn, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_style_radius(btnDn, 16, 0);
  lv_obj_set_style_bg_color(btnDn, lv_color_hex(0x2C2C2E), 0);
  lv_obj_set_style_bg_opa(btnDn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btnDn, lv_color_hex(0x48484A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btnDn, 0, 0);
  lv_obj_set_style_shadow_width(btnDn, 0, 0);
  lv_obj_add_event_cb(btnDn, event_vol_down, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblDn = lv_label_create(btnDn);
  lv_label_set_text(lblDn, "-");
  lv_obj_set_style_text_font(lblDn, &sf_pro_48, 0);
  lv_obj_set_style_text_color(lblDn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(lblDn, LV_ALIGN_CENTER, 0, -6);

  return d;
}

// --- Favorites network + home screen ---


static bool fetch_favorites() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String(BRIDGE_BASE) + "/api/sonos/favorites?room=" + url_encode(gActiveRoom);
  http.begin(url); http.setTimeout(6000);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  String payload = http.getString(); http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;

  gFavCount = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (gFavCount >= MAX_FAVORITES) break;
    FavItem& f = gFavs[gFavCount];
    const char* newUrl = v["artUrl"] | "";
    if (strcmp(f.artUrl, newUrl) != 0) f.artLoaded = false;  // invalidate if URL changed
    strlcpy(f.title,  v["title"]  | "", sizeof(f.title));
    strlcpy(f.artUrl, newUrl, sizeof(f.artUrl));
    f.index       = v["index"] | gFavCount;
    f.canvas      = nullptr;
    f.placeholder = nullptr;
    f.card        = nullptr;
    gFavCount++;
  }

  return true;
}

static void event_fav_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  gPendingFavIdx = (int)(intptr_t)lv_event_get_user_data(e);
  lv_scr_load_anim(screenMain, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
}

static void event_linein_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  gPendingLineIn = true;
  lv_scr_load_anim(screenMain, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
}

static void fetch_fav_art(int idx) {
  if (idx < 0 || idx >= gFavCount) return;
  FavItem& fav = gFavs[idx];
  if (!fav.artBuf || strlen(fav.artUrl) == 0) return;
  // If art is already decoded and in the buffer, just make sure the canvas shows it
  if (fav.artLoaded && fav.canvas && fav.placeholder) {
    lv_canvas_set_buffer(fav.canvas, fav.artBuf, FAV_THUMB_SIZE, FAV_THUMB_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(fav.placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fav.canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(fav.canvas);
    return;
  }
  if (!fav.placeholder) return;

  String url = String(fav.artUrl) + "&size=" + String(FAV_THUMB_SIZE);
  HTTPClient http;
  http.begin(url); http.setTimeout(8000);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return; }

  const size_t maxBuf = 20000;
  uint8_t* jpegBuf = (uint8_t*)ps_malloc(maxBuf);
  if (!jpegBuf) { http.end(); return; }

  WiFiClient* stream = http.getStreamPtr();
  size_t totalRead = 0;
  unsigned long dl = millis() + 8000;
  while (millis() < dl && totalRead < maxBuf) {
    int avail = stream->available();
    if (avail > 0) totalRead += stream->readBytes(jpegBuf + totalRead, min((size_t)avail, maxBuf - totalRead));
    else if (!http.connected()) break;
    else delay(1);
  }
  http.end();
  if (totalRead == 0) { free(jpegBuf); return; }

  uint16_t fill = lv_color_hex(0x292929).full;
  for (size_t i = 0; i < (size_t)FAV_THUMB_SIZE * FAV_THUMB_SIZE; i++) fav.artBuf[i].full = fill;

  gFavArtDst = fav.artBuf;
  TJpgDec.setJpgScale(1);
  TJpgDec.setCallback(fav_jpeg_cb);
  TJpgDec.drawJpg(0, 0, jpegBuf, (uint32_t)totalRead);
  gFavArtDst = nullptr;
  TJpgDec.setCallback(jpeg_cb);
  free(jpegBuf);

  fav.artLoaded = true;
  if (fav.canvas && fav.placeholder) {
    lv_canvas_set_buffer(fav.canvas, fav.artBuf, FAV_THUMB_SIZE, FAV_THUMB_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_add_flag(fav.placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(fav.canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(fav.canvas);
  }
}

static void build_fav_cards() {
  if (!favGrid) return;
  fetch_favorites();
  lv_obj_clean(favGrid);

  const int ROW_W   = 448;
  const int ROW_H   = 80;
  const int ROW_X   = 16;
  const int ROW_GAP = 8;
  const int ROW_PAD = 18;
  const int PAD     = 8;           // uniform padding on all sides
  const int THUMB   = FAV_THUMB_SIZE;
  const int GAP     = 16;          // gap between thumb and title
  const int TITLE_X = PAD + THUMB + GAP;   // 8+64+16 = 88
  const int TITLE_W = ROW_W - TITLE_X - PAD; // 448-88-8 = 352

  // Hardcoded HDMI / line-in row (always slot 0)
  {
    lv_obj_t* row = lv_obj_create(favGrid);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, ROW_X, ROW_PAD);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_clip_corner(row, true, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_event_cb(row, event_linein_tapped, LV_EVENT_CLICKED, NULL);

    lv_obj_t* ph = lv_obj_create(row);
    lv_obj_set_size(ph, THUMB, THUMB);
    lv_obj_set_pos(ph, PAD, PAD);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ph, 10, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0x292929), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_pad_all(ph, 0, 0);
    lv_obj_t* phIco = lv_label_create(ph);
    lv_label_set_text(phIco, MI_TV);
    lv_obj_set_style_text_font(phIco, &lv_font_material_38, 0);
    lv_obj_set_style_text_color(phIco, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(phIco, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* titleLbl = lv_label_create(row);
    lv_label_set_text(titleLbl, "HDMI Input");
    lv_obj_set_width(titleLbl, TITLE_W);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(titleLbl, &sf_pro_26, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(titleLbl, TITLE_X, 0);
    lv_obj_set_height(titleLbl, ROW_H);
    lv_obj_set_style_text_align(titleLbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(titleLbl, (ROW_H - 26) / 2, 0);
  }

  int count = min(gFavCount, MAX_FAVORITES);
  for (int i = 0; i < count; i++) {
    int row_y = ROW_PAD + (i + 1) * (ROW_H + ROW_GAP);  // +1 to make room for HDMI row
    lv_obj_t* row = lv_obj_create(favGrid);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, ROW_X, row_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_clip_corner(row, true, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_event_cb(row, event_fav_tapped, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    gFavs[i].card = row;

    // Placeholder: music note icon, shown until art loads
    lv_obj_t* ph = lv_obj_create(row);
    lv_obj_set_size(ph, THUMB, THUMB);
    lv_obj_set_pos(ph, PAD, PAD);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ph, 10, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0x292929), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_pad_all(ph, 0, 0);
    lv_obj_t* phIco = lv_label_create(ph);
    lv_label_set_text(phIco, MI_MUSIC_NOTE);
    lv_obj_set_style_text_font(phIco, &lv_font_material_26, 0);
    lv_obj_set_style_text_color(phIco, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(phIco, LV_ALIGN_CENTER, 0, 0);
    gFavs[i].placeholder = ph;

    // Canvas: shows decoded art, initially hidden
    // Allocate PSRAM buffer on demand (not in setup) to keep boot path clean
    if (!gFavs[i].artBuf)
      gFavs[i].artBuf = (lv_color_t*)ps_malloc((size_t)FAV_THUMB_SIZE * FAV_THUMB_SIZE * sizeof(lv_color_t));
    lv_obj_t* cv = nullptr;
    if (gFavs[i].artBuf) {
      cv = lv_canvas_create(row);
      lv_obj_set_size(cv, THUMB, THUMB);
      lv_obj_set_pos(cv, PAD, PAD);
      lv_canvas_set_buffer(cv, gFavs[i].artBuf, THUMB, THUMB, LV_IMG_CF_TRUE_COLOR);
      if (gFavs[i].artLoaded) {
        lv_obj_clear_flag(cv, LV_OBJ_FLAG_HIDDEN);   // show immediately from cache
        lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
      }
    }
    gFavs[i].canvas = cv;

    // Title label
    lv_obj_t* titleLbl = lv_label_create(row);
    lv_label_set_text(titleLbl, gFavs[i].title);
    lv_obj_set_width(titleLbl, TITLE_W);
    lv_label_set_long_mode(titleLbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(titleLbl, &sf_pro_26, 0);
    lv_obj_set_style_text_color(titleLbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(titleLbl, TITLE_X, 0);
    lv_obj_set_height(titleLbl, ROW_H);
    lv_obj_set_style_text_align(titleLbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(titleLbl, (ROW_H - 26) / 2, 0);
  }
  if (gFavCount > 0) {
    gFavsLoaded = true;   // only lock in once we actually got favorites
    gFavLoadStep = 2;     // start deferred art loading
  } else {
    gFavLoadStep = 0;     // retry next home screen visit
  }
}


// --- Room picker logic ---
static bool fetch_rooms() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/zones"); http.setTimeout(5000);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return false; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  gRoomCount = 0;
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (gRoomCount >= MAX_ROOMS) break;
    strlcpy(gRooms[gRoomCount].name,        v["name"]        | "", sizeof(gRooms[0].name));
    strlcpy(gRooms[gRoomCount].coordinator, v["coordinator"] | v["name"] | "", sizeof(gRooms[0].coordinator));
    gRooms[gRoomCount].indicator = nullptr;
    gRooms[gRoomCount].nameLabel = nullptr;
    gRooms[gRoomCount].volLabel  = nullptr;
    gRooms[gRoomCount].volMinus  = nullptr;
    gRooms[gRoomCount].volPlus   = nullptr;
    gRooms[gRoomCount].volume    = v["volume"] | 0;
    gRoomCount++;
  }
  // Sort: Living Room always first
  for (int i = 1; i < gRoomCount; i++) {
    if (strcasecmp(gRooms[i].name, "Living Room") == 0) {
      RoomItem tmp = gRooms[0]; gRooms[0] = gRooms[i]; gRooms[i] = tmp;
      break;
    }
  }
  // Determine which rooms are grouped with gActiveRoom (same Sonos zone coordinator)
  char activeCoord[64] = "";
  for (int i = 0; i < gRoomCount; i++) {
    if (strcmp(gRooms[i].name, gActiveRoom) == 0) {
      strlcpy(activeCoord, gRooms[i].coordinator, sizeof(activeCoord));
      break;
    }
  }
  bool anySelected = false;
  for (int i = 0; i < gRoomCount; i++) {
    gRoomSelected[i] = (strlen(activeCoord) > 0 && strcmp(gRooms[i].coordinator, activeCoord) == 0);
    if (gRoomSelected[i]) anySelected = true;
  }
  if (!anySelected && gRoomCount > 0) { gRoomSelected[0] = true; strlcpy(gActiveRoom, gRooms[0].name, sizeof(gActiveRoom)); }
  gRoomVolsFetched = true;
  return gRoomCount > 0;
}

static void fetch_room_volumes() {
  if (WiFi.status() != WL_CONNECTED || gRoomCount == 0) return;
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/room-volumes");
  http.setTimeout(4000);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  for (int i = 0; i < gRoomCount; i++) {
    JsonVariant v = doc[gRooms[i].name];
    if (!v.isNull()) gRooms[i].volume = v | 0;
  }
  gRoomVolsFetched = true;
}

// Refreshes grouping + volumes from Sonos without rebuilding UI objects.
// Call on rooms-screen revisit so external Sonos changes are reflected.
static void refresh_room_state() {
  if (WiFi.status() != WL_CONNECTED || gRoomCount == 0) return;
  HTTPClient http;
  http.begin(String(BRIDGE_BASE) + "/api/sonos/zones");
  http.setTimeout(5000);
  if (http.GET() != HTTP_CODE_OK) { http.end(); return; }
  String payload = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;

  // Find coordinator of the active room in fresh data
  char activeCoord[64] = "";
  for (JsonVariant v : doc.as<JsonArray>()) {
    if (strcmp(v["name"] | "", gActiveRoom) == 0) {
      strlcpy(activeCoord, v["coordinator"] | "", sizeof(activeCoord));
      break;
    }
  }

  // Update volume and selection for each known room
  for (int i = 0; i < gRoomCount; i++) {
    for (JsonVariant v : doc.as<JsonArray>()) {
      if (strcmp(v["name"] | "", gRooms[i].name) != 0) continue;
      gRooms[i].volume = v["volume"] | gRooms[i].volume;
      gRoomSelected[i] = (strlen(activeCoord) > 0 &&
                          strcmp(v["coordinator"] | "", activeCoord) == 0);
      break;
    }
  }
  gRoomVolsFetched = true;
}

static void update_room_indicators() {
  for (int i = 0; i < gRoomCount; i++) {
    bool sel = gRoomSelected[i];
    uint32_t volCol = sel ? 0xAAAAAA : 0x404040;
    if (gRooms[i].indicator) {
      lv_label_set_text(gRooms[i].indicator, sel ? MI_CHECK_CIR : MI_RADIO_OFF);
      lv_obj_set_style_text_color(gRooms[i].indicator,
        sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);
    }
    if (gRooms[i].nameLabel)
      lv_obj_set_style_text_color(gRooms[i].nameLabel,
        sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);
    if (gRooms[i].volLabel) {
      lv_label_set_text_fmt(gRooms[i].volLabel, "%d", gRooms[i].volume);
      lv_obj_set_style_text_color(gRooms[i].volLabel, lv_color_hex(volCol), 0);
    }
    // Dim vol +/− labels when room is not active
    if (gRooms[i].volMinus) {
      lv_obj_t* lbl = lv_obj_get_child(gRooms[i].volMinus, 0);
      if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(volCol), 0);
    }
    if (gRooms[i].volPlus) {
      lv_obj_t* lbl = lv_obj_get_child(gRooms[i].volPlus, 0);
      if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(volCol), 0);
    }
  }
}

static void event_room_tapped(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= gRoomCount) return;
  bool wasSelected = gRoomSelected[idx];
  // Prevent deselecting the last room
  if (wasSelected) {
    int selectedCount = 0;
    for (int i = 0; i < gRoomCount; i++) if (gRoomSelected[i]) selectedCount++;
    if (selectedCount <= 1) return;
  }
  gRoomSelected[idx] = !wasSelected;
  // Keep gActiveRoom as first selected room (for NP display / favorites)
  char prevActive[64]; strlcpy(prevActive, gActiveRoom, sizeof(prevActive));
  for (int i = 0; i < gRoomCount; i++) {
    if (gRoomSelected[i]) { strlcpy(gActiveRoom, gRooms[i].name, sizeof(gActiveRoom)); break; }
  }
  // Only invalidate favorites cache if the primary (NP) room actually changed
  if (strcmp(prevActive, gActiveRoom) != 0) { gFavsLoaded = false; gFavLoadStep = 1; lastArtTrack = ""; }
  // Sync Sonos group: join newly-selected room to coordinator, leave newly-deselected room
  if (!wasSelected && strcmp(gRooms[idx].name, gActiveRoom) != 0) {
    post_room_join(gRooms[idx].name, gActiveRoom);
    // Force coordinator to play so newly joined room syncs immediately
    post_sonos_command_to(gActiveRoom, "play", 0, false);
  } else if (wasSelected) {
    post_room_leave(gRooms[idx].name);
  }
  update_room_indicators();
}

static void event_room_vol_up(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= gRoomCount) return;
  if (!gRoomSelected[idx]) return;
  int newVol = min(100, gRooms[idx].volume + 1);
  gRooms[idx].volume = newVol;
  if (gRooms[idx].volLabel) lv_label_set_text_fmt(gRooms[idx].volLabel, "%d", newVol);
  post_sonos_command_to(gRooms[idx].name, "volume", newVol, true);
  lastActivityMs = millis();
}

static void event_room_vol_down(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  if (gSwipeConsumed) { gSwipeConsumed = false; return; }
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= gRoomCount) return;
  if (!gRoomSelected[idx]) return;
  int newVol = max(0, gRooms[idx].volume - 1);
  gRooms[idx].volume = newVol;
  if (gRooms[idx].volLabel) lv_label_set_text_fmt(gRooms[idx].volLabel, "%d", newVol);
  post_sonos_command_to(gRooms[idx].name, "volume", newVol, true);
  lastActivityMs = millis();
}

static void build_room_cards() {
  if (!roomGrid) return;
  fetch_rooms();  // fetches zones: names, coordinators, volumes, and selection state
  lv_obj_clean(roomGrid);

  const int ROW_W    = 448;
  const int ROW_X    = 16;
  const int ROW_GAP  = 8;
  const int ROW_PAD  = 16;
  // Rows expand to fill the full screen height
  const int ROW_H    = gRoomCount > 0
    ? (LCD_V_RES - 2 * ROW_PAD - (gRoomCount - 1) * ROW_GAP) / gRoomCount
    : 80;
  const int ICON_X   = 20;
  const int ICON_S   = 36;
  const int GAP      = 16;
  const int TITLE_X  = ICON_X + ICON_S + GAP;  // 72

  // Volume controls: [−] vol_num [+] flush to right with 8px margin
  const int VBTN_W   = 40;
  const int VNUM_W   = 40;
  const int VPLUS_X  = ROW_W - 8 - VBTN_W;            // 400
  const int VNUM_X   = VPLUS_X - 4 - VNUM_W;           // 356
  const int VMINUS_X = VNUM_X - 4 - VBTN_W;            // 312
  const int TITLE_W  = VMINUS_X - TITLE_X - 8;         // 232

  for (int i = 0; i < gRoomCount; i++) {
    bool sel = gRoomSelected[i];
    lv_obj_t* row = lv_obj_create(roomGrid);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, ROW_X, ROW_PAD + i * (ROW_H + ROW_GAP));
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t* ico = lv_label_create(row);
    lv_label_set_text(ico, sel ? MI_CHECK_CIR : MI_RADIO_OFF);
    lv_obj_set_style_text_font(ico, &lv_font_material_36, 0);
    lv_obj_set_style_text_color(ico, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);
    lv_obj_set_pos(ico, ICON_X, (ROW_H - ICON_S) / 2);
    gRooms[i].indicator = ico;

    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, gRooms[i].name);
    lv_obj_set_width(lbl, TITLE_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl, &sf_pro_26, 0);
    lv_obj_set_style_text_color(lbl, sel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x757575), 0);
    lv_obj_set_pos(lbl, TITLE_X, 0);
    lv_obj_set_height(lbl, ROW_H);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(lbl, (ROW_H - 26) / 2, 0);
    gRooms[i].nameLabel = lbl;

    // ── Per-room volume: [−] number [+] ──
    uint32_t volCol = sel ? 0xAAAAAA : 0x404040;

    lv_obj_t* btnMinus = make_clean_btn(row);
    lv_obj_set_size(btnMinus, VBTN_W, ROW_H);
    lv_obj_set_pos(btnMinus, VMINUS_X, 0);
    lv_obj_set_style_border_width(btnMinus, 0, 0);
    lv_obj_set_style_shadow_width(btnMinus, 0, 0);
    lv_obj_set_style_pad_all(btnMinus, 0, 0);
    lv_obj_add_event_cb(btnMinus, event_room_vol_down, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* lblM = lv_label_create(btnMinus);
    lv_label_set_text(lblM, "-");
    lv_obj_set_style_text_font(lblM, &sf_pro_26, 0);
    lv_obj_set_style_text_color(lblM, lv_color_hex(volCol), 0);
    lv_obj_center(lblM);
    apply_press_style(btnMinus);
    gRooms[i].volMinus = btnMinus;

    lv_obj_t* volLbl = lv_label_create(row);
    lv_label_set_text_fmt(volLbl, "%d", gRooms[i].volume);
    lv_obj_set_pos(volLbl, VNUM_X, 0);
    lv_obj_set_size(volLbl, VNUM_W, ROW_H);
    lv_obj_set_style_text_font(volLbl, &sf_pro_26, 0);
    lv_obj_set_style_text_color(volLbl, lv_color_hex(volCol), 0);
    lv_obj_set_style_text_align(volLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(volLbl, (ROW_H - 26) / 2, 0);
    gRooms[i].volLabel = volLbl;

    lv_obj_t* btnPlus = make_clean_btn(row);
    lv_obj_set_size(btnPlus, VBTN_W, ROW_H);
    lv_obj_set_pos(btnPlus, VPLUS_X, 0);
    lv_obj_set_style_border_width(btnPlus, 0, 0);
    lv_obj_set_style_shadow_width(btnPlus, 0, 0);
    lv_obj_set_style_pad_all(btnPlus, 0, 0);
    lv_obj_add_event_cb(btnPlus, event_room_vol_up, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    lv_obj_t* lblP = lv_label_create(btnPlus);
    lv_label_set_text(lblP, "+");
    lv_obj_set_style_text_font(lblP, &sf_pro_26, 0);
    lv_obj_set_style_text_color(lblP, lv_color_hex(volCol), 0);
    lv_obj_center(lblP);
    apply_press_style(btnPlus);
    gRooms[i].volPlus = btnPlus;

    // Transparent hit zone covering the left half of the row for room select/deselect.
    // Created last so it sits on top of the icon and name label.
    lv_obj_t* selZone = lv_obj_create(row);
    lv_obj_set_pos(selZone, 0, 0);
    lv_obj_set_size(selZone, ROW_W / 2, ROW_H);
    lv_obj_clear_flag(selZone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(selZone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(selZone, 0, 0);
    lv_obj_set_style_shadow_width(selZone, 0, 0);
    lv_obj_set_style_pad_all(selZone, 0, 0);
    lv_obj_add_event_cb(selZone, event_room_tapped, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  gRoomsLoaded = true;
}

static void event_rooms_back(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  lv_scr_load_anim(screenMain, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
}

static void event_goto_rooms(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  nav_to_rooms();
}

static void nav_to_home() {
  if (!gFavsLoaded) gFavLoadStep = 1;
  lv_scr_load_anim(screenHome, LV_SCR_LOAD_ANIM_OVER_RIGHT, 200, 0, false);
  lastActivityMs = millis();
}

static void nav_to_player() {
  // Direction depends on which screen we're coming from:
  // Home is LEFT of Now Playing → slide left (Home exits left, NP enters from right)
  // Rooms is RIGHT of Now Playing → slide right (Rooms exits right, NP enters from left)
  lv_scr_load_anim_t anim = (lv_scr_act() == screenRooms)
    ? LV_SCR_LOAD_ANIM_OVER_RIGHT
    : LV_SCR_LOAD_ANIM_OVER_LEFT;
  lv_scr_load_anim(screenMain, anim, 200, 0, false);
  lastActivityMs = millis();
}

static void nav_to_rooms() {
  if (!gRoomsLoaded) build_room_cards();
  else { refresh_room_state(); update_room_indicators(); }
  lv_scr_load_anim(screenRooms, LV_SCR_LOAD_ANIM_OVER_LEFT, 200, 0, false);
  lastActivityMs = millis();
}

static void event_goto_home(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  nav_to_home();
}

static void event_goto_player(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  nav_to_player();
}


static lv_obj_t* make_icon_btn(lv_obj_t* parent, const char* sym, int w, int h, const lv_font_t* font);

static void create_boot_screen() {
  screenBoot = lv_obj_create(NULL);
  lv_obj_set_size(screenBoot, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(screenBoot, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screenBoot, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screenBoot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_width(screenBoot, 0, 0);
  lv_obj_set_style_pad_all(screenBoot, 0, 0);

  lv_obj_t* spinner = lv_spinner_create(screenBoot, 1000, 90);
  lv_obj_set_size(spinner, 60, 60);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -16);
  lv_obj_set_style_bg_opa(spinner, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(spinner, 0, 0);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x303030), LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

  bootStatusLbl = lv_label_create(screenBoot);
  lv_label_set_text(bootStatusLbl, "Connecting");
  lv_obj_set_style_text_font(bootStatusLbl, &sf_pro_26, 0);
  lv_obj_set_style_text_color(bootStatusLbl, lv_color_hex(0x404040), 0);
  lv_obj_align(bootStatusLbl, LV_ALIGN_CENTER, 0, 22);

  lv_scr_load(screenBoot);
}

static void create_rooms_ui() {
  screenRooms = lv_obj_create(NULL);
  lv_obj_clear_flag(screenRooms, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(screenRooms, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(screenRooms, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screenRooms, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(screenRooms, 0, 0);
  lv_obj_set_style_pad_all(screenRooms, 0, 0);

  // No header — full screen grid, navigate back via swipe right
  roomGrid = lv_obj_create(screenRooms);
  lv_obj_set_pos(roomGrid, 0, 0);
  lv_obj_set_size(roomGrid, LCD_H_RES, LCD_V_RES);
  lv_obj_set_scroll_dir(roomGrid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(roomGrid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(roomGrid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(roomGrid, 0, 0);
  lv_obj_set_style_pad_all(roomGrid, 0, 0);
  lv_obj_add_flag(roomGrid, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void create_home_ui() {
  screenHome = lv_obj_create(NULL);
  lv_obj_clear_flag(screenHome, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(screenHome, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(screenHome, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screenHome, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(screenHome, 0, 0);
  lv_obj_set_style_pad_all(screenHome, 0, 0);

  // ── Favorites list container (scrollable, full screen — navigate via swipe) ──
  favGrid = lv_obj_create(screenHome);
  lv_obj_set_pos(favGrid, 0, 0);
  lv_obj_set_size(favGrid, LCD_H_RES, LCD_V_RES);
  lv_obj_set_scroll_dir(favGrid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(favGrid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(favGrid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(favGrid, 0, 0);
  lv_obj_set_style_pad_all(favGrid, 0, 0);

  // Volume drawer for home screen
  volumeDrawerHome = make_volume_drawer(screenHome, &labelVolHome);
  lv_obj_add_flag(favGrid, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(screenHome, event_screen_clicked, LV_EVENT_CLICKED, NULL);
}

// --- UI construction ---
//
// Layout (480x320, landscape):
//
//  ┌──────────────────────────── 480 ───────────────────────────────────┐
//  │  [⌂]           Now Playing                        [≡] [🔊]   │ 50
//  ├────────────────────────────────────────────────────────────────────┤  1
//  │16 ╔═══════════════╗16╔══════════════════════════════╗ 16      │
//  │   ║               ║  ║  Song Title  (montserrat_22) ║          │
//  │   ║  Album Art    ║  ║  Artist      (montserrat_18) ║          │
//  │   ║  237 × 237    ║  ║                              ║          │
//  │   ║               ║  ║    [|◄]   [▶/‖]   [►|]      ║          │
//  │   ║               ║  ║                              ║          │
//  │   ║               ║  ║  ─────────────────────────── ║          │
//  │   ║               ║  ║  3:44                  4:05  ║          │
//  │16 ╚═══════════════╝16╚══════════════════════════════╝ 16      │
//  └────────────────────────────────────────────────────────────────────┘
//  Both sections: y=67, h=237, 16px padding on all outer edges and between.

static void apply_press_style(lv_obj_t* btn) {
  lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x404040), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
}

static lv_obj_t* make_icon_btn(lv_obj_t* parent, const char* sym, int w, int h, const lv_font_t* font) {
  lv_obj_t* btn = make_clean_btn(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, sym);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lbl);
  apply_press_style(btn);
  return btn;
}

static void create_ui() {
  // ── Screen ──
  // Figma: 480×320, bg #0e0e0e
  screenMain = lv_obj_create(NULL);
  lv_obj_clear_flag(screenMain, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(screenMain, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(screenMain, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screenMain, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(screenMain, 0, 0);
  lv_obj_set_style_pad_all(screenMain, 0, 0);

  // Header buttons removed — navigate to Home (swipe right) or Rooms (swipe left)

  // ── Album art ──
  // Art expands to fill the space vacated by the removed header:
  // y starts at 24 (same as side padding). Width grows from the original 164
  // by the header height (54px) → 218. Bottom stays near the original position.
  const int ART_W = 218;
  const int ART_X = 24;
  const int ART_Y = 34;

  artClipBox = lv_obj_create(screenMain);
  lv_obj_set_size(artClipBox, ART_W, ART_W);
  lv_obj_set_pos(artClipBox, ART_X, ART_Y);
  lv_obj_clear_flag(artClipBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(artClipBox, 12, 0);
  lv_obj_set_style_clip_corner(artClipBox, true, 0);
  lv_obj_set_style_bg_opa(artClipBox, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(artClipBox, 0, 0);
  lv_obj_set_style_shadow_width(artClipBox, 0, 0);
  lv_obj_set_style_pad_all(artClipBox, 0, 0);

  artPlaceholder = lv_obj_create(artClipBox);
  lv_obj_set_size(artPlaceholder, ART_W, ART_W);
  lv_obj_set_pos(artPlaceholder, 0, 0);
  lv_obj_clear_flag(artPlaceholder, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(artPlaceholder, 0, 0);
  // Figma token: --state-layers/on-secondary/opacity-16 = rgba(255,255,255,0.16) on black
  lv_obj_set_style_bg_color(artPlaceholder, lv_color_hex(0x292929), 0);
  lv_obj_set_style_bg_opa(artPlaceholder, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(artPlaceholder, 0, 0);
  lv_obj_set_style_shadow_width(artPlaceholder, 0, 0);
  lv_obj_set_style_pad_all(artPlaceholder, 0, 0);
  lv_obj_t* phIcon = lv_label_create(artPlaceholder);
  lv_label_set_text(phIcon, MI_MUSIC_NOTE);
  lv_obj_set_style_text_font(phIcon, &lv_font_material_82, 0);
  lv_obj_set_style_text_color(phIcon, lv_color_hex(0x9E9E9E), 0);
  lv_obj_align(phIcon, LV_ALIGN_CENTER, 0, 0);

  artCanvas = lv_canvas_create(artClipBox);
  lv_obj_set_size(artCanvas, ART_W, ART_W);
  lv_obj_set_pos(artCanvas, 0, 0);
  lv_canvas_set_buffer(artCanvas, artCanvasBuf, ART_SIZE, ART_SIZE, LV_IMG_CF_TRUE_COLOR);
  lv_obj_add_flag(artCanvas, LV_OBJ_FLAG_HIDDEN);

  // ── Song title ──
  // RX = ART_X + ART_W + 8px gap = 250. RW = 480 - 250 - 24 = 206.
  const int RX = 250;
  const int RW = LCD_H_RES - RX - 24;  // 206

  labelSongTitle = lv_label_create(screenMain);
  lv_obj_set_pos(labelSongTitle, RX + 16, 70);
  lv_obj_set_width(labelSongTitle, RW - 16);
  lv_label_set_long_mode(labelSongTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(labelSongTitle, "Nothing Playing");
  lv_obj_set_style_text_font(labelSongTitle, &sf_pro_32, 0);
  lv_obj_set_style_text_color(labelSongTitle, lv_color_hex(0xFFFFFF), 0);

  // ── Artist ──
  // Figma: x=222, y=120, font 20px, color #f2f2f7
  labelArtist = lv_label_create(screenMain);
  lv_obj_set_pos(labelArtist, RX + 16, 112);
  lv_obj_set_width(labelArtist, RW - 16);
  lv_label_set_long_mode(labelArtist, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(labelArtist, "");
  lv_obj_set_style_text_font(labelArtist, &sf_pro_22, 0);
  lv_obj_set_style_text_color(labelArtist, lv_color_hex(0xf2f2f7), 0);

  // ── Transport controls ──
  // Figma: 48px icons in 64px circular buttons.
  // Hover/pressed token: --state-layers/on-secondary/opacity-16 = rgba(255,255,255,0.16) on black
  // Positions: skip_prev x=203, play x=301, skip_next x=399 (all y=178)
  // Gaps of ~34px between buttons; skip_next right edge ≈16px from screen edge.
  const int BTN_W  = 64;   // button hit target
  const int BTN_Y  = 164;
  // Three buttons evenly spaced from RX to the 24px right margin.
  // RX=250, right=456: span=206. Spacing=(206-3×64)/2=7px.
  const int TBTN_X0 = 250;  // skip_prev
  const int TBTN_X1 = 321;  // play/pause  (250+64+7)
  const int TBTN_X2 = 392;  // skip_next   (321+64+7)
  btnPrev = make_clean_btn(screenMain);
  lv_obj_set_size(btnPrev, BTN_W, BTN_W);
  lv_obj_set_pos(btnPrev, TBTN_X0, BTN_Y);
  lv_obj_set_style_border_width(btnPrev, 0, 0);
  lv_obj_set_style_shadow_width(btnPrev, 0, 0);
  lv_obj_set_style_pad_all(btnPrev, 0, 0);
  lv_obj_add_event_cb(btnPrev, event_prev, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblPrev = lv_label_create(btnPrev);
  lv_label_set_text(lblPrev, MI_SKIP_PREV);
  lv_obj_set_style_text_font(lblPrev, &lv_font_material_48, 0);
  lv_obj_set_style_text_color(lblPrev, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lblPrev);
  apply_press_style(btnPrev);

  btnPlayPause = make_clean_btn(screenMain);
  lv_obj_set_size(btnPlayPause, BTN_W, BTN_W);
  lv_obj_set_pos(btnPlayPause, TBTN_X1, BTN_Y);
  lv_obj_set_style_border_width(btnPlayPause, 0, 0);
  lv_obj_set_style_shadow_width(btnPlayPause, 0, 0);
  lv_obj_set_style_radius(btnPlayPause, BTN_W / 2, 0);
  lv_obj_add_event_cb(btnPlayPause, event_play_pause, LV_EVENT_CLICKED, NULL);
  labelPlayPause = lv_label_create(btnPlayPause);
  lv_label_set_text(labelPlayPause, MI_PLAY);
  lv_obj_set_style_text_font(labelPlayPause, &lv_font_material_48, 0);
  lv_obj_set_style_text_color(labelPlayPause, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(labelPlayPause);
  apply_press_style(btnPlayPause);

  btnNext = make_clean_btn(screenMain);
  lv_obj_set_size(btnNext, BTN_W, BTN_W);
  lv_obj_set_pos(btnNext, TBTN_X2, BTN_Y);
  lv_obj_set_style_border_width(btnNext, 0, 0);
  lv_obj_set_style_shadow_width(btnNext, 0, 0);
  lv_obj_set_style_radius(btnNext, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnNext, 0, 0);
  lv_obj_add_event_cb(btnNext, event_next, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lblNext = lv_label_create(btnNext);
  lv_label_set_text(lblNext, MI_SKIP_NEXT);
  lv_obj_set_style_text_font(lblNext, &lv_font_material_48, 0);
  lv_obj_set_style_text_color(lblNext, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lblNext);
  apply_press_style(btnNext);

  // ── Shuffle / Repeat buttons (hidden by default, shown when art/title tapped) ──
  // Left-justified pair starting at skip_prev position with a 16px gap between
  const int PM_X0 = TBTN_X0;   // shuffle — same slot as skip_prev
  const int PM_X1 = TBTN_X1;   // repeat  — same slot as play/pause
  const int PM_X2 = TBTN_X2;   // TV      — same slot as skip_next

  btnShuffle = make_clean_btn(screenMain);
  lv_obj_set_size(btnShuffle, BTN_W, BTN_W);
  lv_obj_set_pos(btnShuffle, PM_X0, BTN_Y);
  lv_obj_set_style_border_width(btnShuffle, 0, 0);
  lv_obj_set_style_shadow_width(btnShuffle, 0, 0);
  lv_obj_set_style_radius(btnShuffle, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnShuffle, 0, 0);
  lv_obj_add_flag(btnShuffle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(btnShuffle, event_shuffle_tapped, LV_EVENT_CLICKED, NULL);
  lblShuffleIcon = lv_label_create(btnShuffle);
  lv_label_set_text(lblShuffleIcon, MI_SHUFFLE);
  lv_obj_set_style_text_font(lblShuffleIcon, &lv_font_material_38, 0);
  lv_obj_set_style_text_color(lblShuffleIcon, lv_color_hex(0x505050), 0);
  lv_obj_center(lblShuffleIcon);
  apply_press_style(btnShuffle);

  btnRepeat = make_clean_btn(screenMain);
  lv_obj_set_size(btnRepeat, BTN_W, BTN_W);
  lv_obj_set_pos(btnRepeat, PM_X1, BTN_Y);
  lv_obj_set_style_border_width(btnRepeat, 0, 0);
  lv_obj_set_style_shadow_width(btnRepeat, 0, 0);
  lv_obj_set_style_radius(btnRepeat, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnRepeat, 0, 0);
  lv_obj_add_flag(btnRepeat, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(btnRepeat, event_repeat_tapped, LV_EVENT_CLICKED, NULL);
  lblRepeatIcon = lv_label_create(btnRepeat);
  lv_label_set_text(lblRepeatIcon, MI_REPEAT);
  lv_obj_set_style_text_font(lblRepeatIcon, &lv_font_material_38, 0);
  lv_obj_set_style_text_color(lblRepeatIcon, lv_color_hex(0x505050), 0);
  lv_obj_center(lblRepeatIcon);
  apply_press_style(btnRepeat);

  btnTV = make_clean_btn(screenMain);
  lv_obj_set_size(btnTV, BTN_W, BTN_W);
  lv_obj_set_pos(btnTV, PM_X2, BTN_Y);
  lv_obj_set_style_border_width(btnTV, 0, 0);
  lv_obj_set_style_shadow_width(btnTV, 0, 0);
  lv_obj_set_style_radius(btnTV, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnTV, 0, 0);
  lv_obj_add_flag(btnTV, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(btnTV, event_tv_tapped, LV_EVENT_CLICKED, NULL);
  lblTVIcon = lv_label_create(btnTV);
  lv_label_set_text(lblTVIcon, MI_TV);
  lv_obj_set_style_text_font(lblTVIcon, &lv_font_material_38, 0);
  lv_obj_set_style_text_color(lblTVIcon, lv_color_hex(0x505050), 0);
  lv_obj_center(lblTVIcon);
  apply_press_style(btnTV);

  // Transparent tap zones covering art + metadata — created last so they sit on top
  // and reliably receive touches that would otherwise be consumed by child objects.
  auto make_tap_zone = [](lv_obj_t* parent, int x, int y, int w, int h, lv_event_cb_t cb) {
    lv_obj_t* z = lv_obj_create(parent);
    lv_obj_set_pos(z, x, y);
    lv_obj_set_size(z, w, h);
    lv_obj_clear_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(z, 0, 0);
    lv_obj_set_style_shadow_width(z, 0, 0);
    lv_obj_set_style_pad_all(z, 0, 0);
    lv_obj_add_event_cb(z, cb, LV_EVENT_CLICKED, NULL);
  };
  make_tap_zone(screenMain, ART_X, ART_Y, ART_W, ART_W, event_playmode_tapped);  // album art
  make_tap_zone(screenMain, RX, 70, RW, 80, event_playmode_tapped);               // song title + artist

  // ── Progress bar (graphical only — no time labels) ──
  const int PROG_X = 24;
  const int PROG_Y = 286;
  const int PROG_W = 432;

  progressSlider = lv_slider_create(screenMain);
  lv_obj_set_size(progressSlider, PROG_W, 4);
  lv_obj_set_pos(progressSlider, PROG_X, PROG_Y);
  lv_slider_set_range(progressSlider, 0, 100);
  lv_slider_set_value(progressSlider, 0, LV_ANIM_OFF);
  lv_obj_clear_flag(progressSlider, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(progressSlider, lv_color_hex(0x787880), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(progressSlider, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_radius(progressSlider, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(progressSlider, lv_color_hex(0xCCCCCC), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(progressSlider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(progressSlider, 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(progressSlider, lv_color_hex(0xCCCCCC), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(progressSlider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(progressSlider, 4, LV_PART_KNOB);
  lv_obj_set_style_radius(progressSlider, 8, LV_PART_KNOB);

  // ── In-place volume controls — same slots as transport buttons ──
  const int VOL_X0 = TBTN_X0;   // 203 — same as skip_prev
  const int VOL_X1 = TBTN_X1;   // 301 — same as play/pause
  const int VOL_X2 = TBTN_X2;   // 399 — same as skip_next

  btnVolDown = make_clean_btn(screenMain);
  lv_obj_set_size(btnVolDown, BTN_W, BTN_W);
  lv_obj_set_pos(btnVolDown, VOL_X0, BTN_Y);
  lv_obj_set_style_border_width(btnVolDown, 0, 0);
  lv_obj_set_style_shadow_width(btnVolDown, 0, 0);
  lv_obj_set_style_radius(btnVolDown, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnVolDown, 0, 0);
  lv_obj_add_event_cb(btnVolDown, event_vol_down, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(btnVolDown, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* lblVD = lv_label_create(btnVolDown);
  lv_label_set_text(lblVD, MI_VOL_MINUS);
  lv_obj_set_style_text_font(lblVD, &lv_font_material_48, 0);
  lv_obj_set_style_text_color(lblVD, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lblVD);
  apply_press_style(btnVolDown);

  // Volume number — clickable to toggle mute
  labelVolDisplay = lv_label_create(screenMain);
  lv_obj_set_pos(labelVolDisplay, VOL_X1, BTN_Y + (BTN_W - 32) / 2);
  lv_obj_set_width(labelVolDisplay, BTN_W);
  lv_obj_set_style_text_align(labelVolDisplay, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(labelVolDisplay, "0");
  lv_obj_set_style_text_font(labelVolDisplay, &sf_pro_32, 0);
  lv_obj_set_style_text_color(labelVolDisplay, lv_color_hex(0xFFFFFF), 0);
  lv_obj_add_flag(labelVolDisplay, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(labelVolDisplay, event_vol_mute, LV_EVENT_CLICKED, NULL);

  btnVolUp = make_clean_btn(screenMain);
  lv_obj_set_size(btnVolUp, BTN_W, BTN_W);
  lv_obj_set_pos(btnVolUp, VOL_X2, BTN_Y);
  lv_obj_set_style_border_width(btnVolUp, 0, 0);
  lv_obj_set_style_shadow_width(btnVolUp, 0, 0);
  lv_obj_set_style_radius(btnVolUp, BTN_W / 2, 0);
  lv_obj_set_style_pad_all(btnVolUp, 0, 0);
  lv_obj_add_event_cb(btnVolUp, event_vol_up, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(btnVolUp, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* lblVU = lv_label_create(btnVolUp);
  lv_label_set_text(lblVU, MI_VOL_PLUS);
  lv_obj_set_style_text_font(lblVU, &lv_font_material_48, 0);
  lv_obj_set_style_text_color(lblVU, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(lblVU);
  apply_press_style(btnVolUp);

}

// --- WiFi ---
static void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    lv_timer_handler(); delay(16);
  }
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk) Serial.println("WiFi connected: " + WiFi.localIP().toString());
  else        Serial.println("WiFi failed");
}

// --- Arduino entry points ---
void setup() {
  Serial.begin(115200);
  pinMode(18, OUTPUT);

  delay(50);

  gfx.init();
  gfx.initDMA();
  gfx.startWrite();
  gfx.fillScreen(TFT_BLACK);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf_a, buf_b, LCD_H_RES * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = LCD_H_RES;
  disp_drv.ver_res  = LCD_V_RES;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  delay(100);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  touch_init(0x14);

  // Allocate PSRAM canvas buffer for player screen art
  artCanvasBuf = (lv_color_t*)ps_malloc((size_t)ART_SIZE * ART_SIZE * sizeof(lv_color_t));
  if (!artCanvasBuf) Serial.println("ERROR: art canvas buffer alloc failed");


  gfx.fillScreen(TFT_BLACK);
  create_boot_screen();       // spinner visible immediately
  lv_timer_handler();         // flush to display before blocking calls

  create_rooms_ui();  lv_timer_handler();
  create_ui();        lv_timer_handler();
  create_home_ui();   lv_timer_handler();
  init_screenshot_buffer();
  connect_wifi();

  if (wifiOk) {
    ArduinoOTA.setHostname("sonos-frame");
    ArduinoOTA.begin();
    screenshotServer.begin();
    Serial.println("OTA + screenshot server ready. IP: " + WiFi.localIP().toString());
    Serial.println("Screenshot: http://sonos-frame.local:8080/screenshot");
    if (bootStatusLbl) lv_label_set_text(bootStatusLbl, "Loading");
    lv_timer_handler();
  }

  lastActivityMs = millis();  // start sleep timer from boot complete

  if (wifiOk && fetch_now_playing()) update_ui();
  else { sanitize_state(); update_ui(); }

  // Fade from boot screen into the populated player screen
  lv_scr_load_anim(screenMain, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
  screenBoot = nullptr; bootStatusLbl = nullptr;

  // Kick off background favorites prefetch so home screen is instant on first visit
  if (wifiOk) gFavLoadStep = 1;

  Serial.println("Setup done. Send 's' over Serial for a BMP screenshot.");
}

void loop() {
  ArduinoOTA.handle();
  lv_timer_handler();

  // Loop-based swipe detection: track horizontal finger travel and navigate on
  // threshold, setting gSwipeConsumed so click handlers ignore the lift event.
  {
    lv_indev_t* indev = lv_indev_get_next(NULL);
    if (indev) {
      lv_point_t pt; lv_indev_get_point(indev, &pt);
      if (indev->proc.state == LV_INDEV_STATE_PR) {
        if (!gSwipeActive) {
          gSwipeStartX = pt.x;
          gSwipeActive = true;
          gSwipeConsumed = false;
        } else if (!gSwipeConsumed) {
          int dx = (int)pt.x - (int)gSwipeStartX;
          if (abs(dx) > 40) {
            gSwipeConsumed = true;
            lv_obj_t* cur = lv_scr_act();
            if (dx > 0) {
              if      (cur == screenMain)  nav_to_home();
              else if (cur == screenRooms) nav_to_player();
            } else {
              if      (cur == screenHome)  nav_to_player();
              else if (cur == screenMain)  nav_to_rooms();
            }
          }
        }
      } else {
        gSwipeActive = false;
      }
    }
  }

  // During screen transitions, dedicate all CPU to rendering — skip HTTP work.
  if (lv_anim_count_running() > 0) return;

  handle_serial_commands();
  handle_screenshot_server();

  if (gTapMode != 0 && millis() - gTapModeMs >= 4000) set_tap_mode(0);

  if (displayOn && millis() - lastActivityMs >= SLEEP_TIMEOUT_MS) set_display(false);

  // Favorites prefetch: runs in background regardless of active screen.
  // build_fav_cards only blocks briefly (widget creation); fetch_fav_art does one HTTP call/tick.
  // art that's already cached (artLoaded=true) returns instantly with no HTTP call.
  if (gFavLoadStep == 1) {
    gFavLoadStep = 0;
    build_fav_cards();          // sets gFavLoadStep = 2 when done
  } else if (gFavLoadStep >= 2 && gFavLoadStep < 2 + gFavCount) {
    int i = gFavLoadStep - 2;
    gFavLoadStep++;
    fetch_fav_art(i);
  }

  if (gPendingLineIn) {
    gPendingLineIn = false;
    post_sonos_command("linein");
    delay(200);
    if (fetch_now_playing()) update_ui();
  }

  if (gPendingFavIdx >= 0) {
    int idx = gPendingFavIdx;
    gPendingFavIdx = -1;
    post_sonos_command("favorite", idx, true);
    delay(200);
    if (fetch_now_playing()) update_ui();
  }

  if (gPendingMute >= 0) {
    int m = gPendingMute;
    gPendingMute = -1;
    post_sonos_command(m ? "mute" : "unmute");
  }

  if (wifiOk && displayOn) {
    bool nearEnd = (gState.durationSec > 0 &&
                    (gState.durationSec - gState.elapsedSec) <= FAST_POLL_END_SEC);
    uint32_t interval = (gFastPollCount > 0 || nearEnd) ? FAST_POLL_MS : POLL_MS;
    if (millis() - lastPollMs >= interval) {
      lastPollMs = millis();
      if (gFastPollCount > 0) gFastPollCount--;
      if (fetch_now_playing()) update_ui();
    }
  }

  if (!wifiOk && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiRetry = 0;
    if (millis() - lastWifiRetry > 5000) {
      lastWifiRetry = millis();
      connect_wifi();
    }
  }

  delay(5);
}
