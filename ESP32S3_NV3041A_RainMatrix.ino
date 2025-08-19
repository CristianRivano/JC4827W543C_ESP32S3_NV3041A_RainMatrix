/* =========================================================
   ESP32-S3 + JC4827W543C (NV3041A 480x272 QSPI)
   "Digital Rain" — Dual‑core con DOBLE BUFFER Ping‑Pong (PSRAM)

   • Core1 = Render fijo (FRAME_MS)
   • Soporta 3 modos automáticamente:
       0) Directo a pantalla (sin PSRAM)
       1) Framebuffer único (si sólo hay 1 buffer)
       2) Ping‑Pong (dos framebuffers y swap por frame)
   • RainEngine dibuja sobre el destino actual (screen / FB) y
     el renderer hace el blit al final del frame si corresponde.
   ========================================================= */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_random.h>
#include <esp_heap_caps.h>  // heap_caps_malloc (PSRAM)

// -------------------- Pines / Hardware --------------------
#define TFT_BL_PIN 1  // Retroiluminación (HIGH = encendido)

// QSPI NV3041A (pines del usuario)
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */
);

// Panel NV3041A (rotación = 0, IPS = true)
Arduino_GFX *gfx = new Arduino_NV3041A(
  bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */
);

// -------------------- Dimensiones / Colores --------------------
constexpr int16_t W = 480;
constexpr int16_t H = 272;

constexpr uint16_t COL_BG      = 0x0000;  // negro
constexpr uint16_t COL_HEAD    = 0xFFFF;  // cabeza blanca
constexpr uint16_t COL_TRAIL   = 0x07E0;  // verde medio
constexpr uint16_t COL_RESIDUE = 0x03E0;  // verde tenue

// -------------------- Caracteres / Animación --------------------
const uint8_t  CHAR_W   = 6;     // 5px glyph + 1px espacio
const uint8_t  CHAR_H   = 8;     // 7px glyph + 1px espacio
const uint16_t FRAME_MS = 60;    // objetivo ~16 FPS
const uint8_t  TRAIL    = 10;    // longitud base de la estela

// -------------------- FreeRTOS --------------------
static TaskHandle_t gRenderTask = nullptr;  // Tarea de render (Core1)
constexpr BaseType_t RENDER_CORE = 1;       // Anclar a Core1

// -------------------- Doble buffer --------------------
// Se elige modo dinámicamente según memoria disponible.
static uint16_t *gFrameA = nullptr;  // Buffer A (RGB565)
static uint16_t *gFrameB = nullptr;  // Buffer B (RGB565)
static uint16_t *gDraw   = nullptr;  // Dónde se dibuja este frame
static uint16_t *gBlit   = nullptr;  // Qué se blitea este frame
static size_t    gFrameSize = W * H * sizeof(uint16_t);  // 480*272*2 = 261120 bytes

// Modo de salida:
// 0 = Direct (sin FB) | 1 = Single FB | 2 = Ping-Pong
static uint8_t gMode = 0;

// =========================================================
//  Fuente 5x7 compacta (ASCII 0x20..0x7E) — columnas de 7 bits
// =========================================================
static const uint8_t PROGMEM font5x7[] = {
  0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14,
  0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00,
  0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x14,0x08,0x3E,0x08,0x14, 0x08,0x08,0x3E,0x08,0x08,
  0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00, 0x20,0x10,0x08,0x04,0x02,
  0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x62,0x51,0x49,0x49,0x46, 0x22,0x41,0x49,0x49,0x36,
  0x18,0x14,0x12,0x7F,0x10, 0x2F,0x49,0x49,0x49,0x31, 0x3E,0x49,0x49,0x49,0x32, 0x01,0x71,0x09,0x05,0x03,
  0x36,0x49,0x49,0x49,0x36, 0x26,0x49,0x49,0x49,0x3E, 0x00,0x36,0x36,0x00,0x00, 0x00,0x56,0x36,0x00,0x00,
  0x08,0x14,0x22,0x41,0x00, 0x14,0x14,0x14,0x14,0x14, 0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06,
  0x3E,0x41,0x5D,0x59,0x4E, 0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
  0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01, 0x3E,0x41,0x49,0x49,0x7A,
  0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41,
  0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x04,0x02,0x7F, 0x3E,0x41,0x41,0x41,0x3E, 0x7F,0x09,0x09,0x09,0x06,
  0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x26,0x49,0x49,0x49,0x32, 0x01,0x01,0x7F,0x01,0x01,
  0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x7F,0x20,0x18,0x20,0x7F, 0x63,0x14,0x08,0x14,0x63,
  0x03,0x04,0x78,0x04,0x03, 0x61,0x51,0x49,0x45,0x43, 0x00,0x7F,0x41,0x41,0x00, 0x02,0x04,0x08,0x10,0x20,
  0x00,0x41,0x41,0x7F,0x00, 0x04,0x02,0x01,0x02,0x04, 0x40,0x40,0x40,0x40,0x40, 0x00,0x01,0x02,0x04,0x00,
  0x20,0x54,0x54,0x54,0x78, 0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x20, 0x38,0x44,0x44,0x48,0x7F,
  0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x0C,0x52,0x52,0x52,0x3E, 0x7F,0x08,0x04,0x04,0x78,
  0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00, 0x7F,0x10,0x28,0x44,0x00, 0x00,0x41,0x7F,0x40,0x00,
  0x7C,0x04,0x18,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38, 0x7C,0x14,0x14,0x14,0x08,
  0x08,0x14,0x14,0x14,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x24, 0x04,0x3F,0x44,0x40,0x20,
  0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, 0x3C,0x40,0x30,0x40,0x3C,  0x44,0x28,0x10,0x28,0x44,
  0x0C,0x50,0x50,0x50,0x3C, 0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00, 0x00,0x00,0x7F,0x00,0x00,
  0x00,0x41,0x36,0x08,0x00, 0x08,0x04,0x08,0x10,0x08
};

// =========================================================
//  Motor de lluvia con soporte de destino: FB o pantalla
// =========================================================
class RainEngine {
public:
  struct Drop { int16_t y; uint8_t speed; bool active; };
  static constexpr int16_t COLS = W / CHAR_W;
  static constexpr int16_t ROWS = H / CHAR_H;

  // Caché de tiles: 95 chars * 3 colores * (6x8 px)
  static uint16_t tileCache[95][3][CHAR_W * CHAR_H];

  enum TileColor : uint8_t { TILE_HEAD = 0, TILE_TRAIL = 1, TILE_RESIDUE = 2 };

  void begin(Arduino_GFX *g, uint16_t *framebuffer) {
    gfx_ = g; fb_ = framebuffer; buildAllTiles();
    for (int c = 0; c < COLS; ++c) { drops[c].active = false; if ((esp_random() & 0x03) == 0) spawnDrop(c); }
  }
  void setTarget(uint16_t *framebuffer) { fb_ = framebuffer; }  // cambia destino en runtime
  void step();
  void clear();
  bool usingFramebuffer() const { return fb_ != nullptr; }

private:
  Drop drops[COLS];
  Arduino_GFX *gfx_ = nullptr;
  uint16_t *fb_ = nullptr;  // si != nullptr, dibuja en framebuffer

  // --- utilidades ---
  static inline uint16_t colorFor(TileColor tc) {
    switch (tc) { case TILE_HEAD: return COL_HEAD; case TILE_TRAIL: return COL_TRAIL; default: return COL_RESIDUE; }
  }
  static inline uint8_t clampCharIndex(int c) { if (c < 0x20) c = 0x20; if (c > 0x7E) c = 0x7E; return uint8_t(c - 0x20); }
  static inline char randChar() { return char(0x20 + (esp_random() % 95)); }

  static void buildTileFromFont(uint8_t charIdx, TileColor tcol) {
    const uint16_t fg = colorFor(tcol); uint16_t *buf = tileCache[charIdx][tcol]; const uint8_t *glyph = &font5x7[charIdx * 5];
    for (uint8_t y = 0; y < CHAR_H; ++y) {
      for (uint8_t x = 0; x < CHAR_W; ++x) {
        uint16_t px = COL_BG;
        if (x < 5 && y < 7) {
          uint8_t colBits = pgm_read_byte(glyph + x);
          bool on = (colBits >> y) & 0x01;
          if (!on && tcol == TILE_HEAD && y > 0) on = (colBits >> (y - 1)) & 0x01;  // engrosar cabeza
          if (on) px = fg;
        }
        buf[y * CHAR_W + x] = px;
      }
    }
  }
  static void buildAllTiles() { for (uint8_t i = 0; i < 95; ++i) { buildTileFromFont(i, TILE_HEAD); buildTileFromFont(i, TILE_TRAIL); buildTileFromFont(i, TILE_RESIDUE); } }

  inline void drawCell_screen(int col, int row, TileColor tcol, char c) {
    const int16_t x = col * CHAR_W; const int16_t y = row * CHAR_H; const uint8_t idx = clampCharIndex((uint8_t)c);
    gfx_->draw16bitRGBBitmap(x, y, tileCache[idx][tcol], CHAR_W, CHAR_H);
  }
  inline void drawCell_fb(int col, int row, TileColor tcol, char c) {
    const int16_t x = col * CHAR_W; const int16_t y = row * CHAR_H; const uint8_t idx = clampCharIndex((uint8_t)c);
    const uint16_t *src = tileCache[idx][tcol]; uint16_t *dst = fb_ + y * W + x;
    for (uint8_t ty = 0; ty < CHAR_H; ++ty) { memcpy(dst + ty * W, src + ty * CHAR_W, CHAR_W * sizeof(uint16_t)); }
  }
  inline void drawCell(int col, int row, TileColor tcol, char c) { if (fb_) drawCell_fb(col, row, tcol, c); else drawCell_screen(col, row, tcol, c); }
  inline void spawnDrop(int col) { drops[col].y = -int(esp_random() % ROWS); drops[col].speed = 1 + (esp_random() % 3); drops[col].active = true; }
};

// --- Definición de estáticos ---
uint16_t RainEngine::tileCache[95][3][CHAR_W * CHAR_H];

void RainEngine::clear() {
  if (fb_) memset(fb_, 0, gFrameSize); else if (gfx_) gfx_->fillScreen(COL_BG);
}

void RainEngine::step() {
  for (int c = 0; c < COLS; ++c) {
    if (!drops[c].active && (esp_random() & 0x07) == 0) spawnDrop(c);
    if (!drops[c].active) continue;

    const int trail_len = TRAIL + (esp_random() % 5);

    const int clearRow = drops[c].y - trail_len - 1;
    if ((uint16_t)clearRow < (uint16_t)ROWS) { drawCell(c, clearRow, TILE_RESIDUE, randChar()); }

    for (int t = 1; t <= trail_len; ++t) {
      const int r = drops[c].y - t;
      if ((uint16_t)r < (uint16_t)ROWS) {
        const TileColor fade_color = (t > trail_len * 0.75f) ? TILE_RESIDUE : TILE_TRAIL;
        drawCell(c, r, fade_color, randChar());
      }
    }

    if ((uint16_t)drops[c].y < (uint16_t)ROWS) { drawCell(c, drops[c].y, TILE_HEAD, randChar()); }

    drops[c].y += drops[c].speed;

    if (drops[c].y - trail_len > ROWS) { if ((esp_random() & 0x03) == 0) spawnDrop(c); else drops[c].active = false; }
  }
}

// =========================================================
//  Instancia global del motor y métricas de FPS
// =========================================================
static RainEngine rain;
static uint32_t gFrames = 0, gAccumMs = 0, gLastSec = 0;

// =========================================================
//  Tarea de render (Core1):
//  • Modo 2 (Ping‑Pong): setTarget(gDraw) → step() → blit(gDraw) → swap
//  • Modo 1 (Single): setTarget(gFrameA)   → step() → blit(gFrameA)
//  • Modo 0 (Direct): setTarget(nullptr)   → step() (dibuja directo)
// =========================================================
static void renderTask(void *arg) {
  (void)arg; gLastSec = millis();
  for (;;) {
    const uint32_t t0 = millis();

    if (gMode == 2) {
      rain.setTarget(gDraw);
      rain.step();
      gfx->draw16bitRGBBitmap(0, 0, gDraw, W, H);
      // swap buffers
      uint16_t *tmp = gDraw; gDraw = gBlit; gBlit = tmp;
    } else if (gMode == 1) {
      rain.setTarget(gFrameA);
      rain.step();
      gfx->draw16bitRGBBitmap(0, 0, gFrameA, W, H);
    } else {
      rain.setTarget(nullptr);
      rain.step();
    }

    const uint32_t dt = millis() - t0; gFrames++; gAccumMs += dt;

    const uint32_t now = millis();
    if (now - gLastSec >= 1000) {
      const uint32_t fps = (gAccumMs ? (1000UL * gFrames / gAccumMs) : 0UL);
      const char *m = (gMode==2?"PingPong":(gMode==1?"SingleFB":"Direct"));
      Serial.printf("[Render] %s | FPS ~ %lu", m, (unsigned long)fps);
      gFrames = 0; gAccumMs = 0; gLastSec = now;
    }

    const int32_t slack_ms = (int32_t)FRAME_MS - (int32_t)dt;
    if (slack_ms > 0) vTaskDelay(pdMS_TO_TICKS(slack_ms)); else taskYIELD();
  }
}

// =========================================================
//  setup() : inicializa HW, decide modo y crea tarea Core1
// =========================================================
void setup() {
  Serial.begin(115200); delay(50);

  pinMode(TFT_BL_PIN, OUTPUT); digitalWrite(TFT_BL_PIN, LOW);

  if (!gfx->begin()) { Serial.println("[ERR] gfx->begin() falló"); while (true) { delay(1000); } }

  // ----- Intentar reservar buffers en PSRAM -----
  if (psramFound()) {
    gFrameA = (uint16_t*) heap_caps_malloc(gFrameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gFrameA) memset(gFrameA, 0x00, gFrameSize);

    gFrameB = (uint16_t*) heap_caps_malloc(gFrameSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gFrameB) memset(gFrameB, 0x00, gFrameSize);

    if (gFrameA && gFrameB) {
      gMode = 2; gDraw = gFrameA; gBlit = gFrameB;
      Serial.println("[INFO] Modo Ping‑Pong (2 FB en PSRAM)");
    } else if (gFrameA) {
      gMode = 1; Serial.println("[INFO] Modo Single FB (1 buffer en PSRAM)");
    } else {
      gMode = 0; Serial.println("[WARN] PSRAM sin buffers útiles. Modo Direct.");
    }
  } else {
    gMode = 0; Serial.println("[WARN] PSRAM no detectada. Modo Direct.");
  }

  digitalWrite(TFT_BL_PIN, HIGH);
  if (gMode == 0) gfx->fillScreen(COL_BG);  // en Direct limpiamos pantalla

  // Iniciar motor con el destino actual
  uint16_t *initialFB = (gMode==2? gDraw : (gMode==1? gFrameA : nullptr));
  rain.begin(gfx, initialFB);
  rain.clear();  // limpia FB o pantalla

  // Crear la tarea de render en Core1
  const uint32_t stackWords = 8192; const UBaseType_t prio = 1;
  BaseType_t ok = xTaskCreatePinnedToCore(renderTask, "RenderTask", stackWords, nullptr, prio, &gRenderTask, RENDER_CORE);
  if (ok != pdPASS) { Serial.println("[ERR] No se pudo crear RenderTask"); while (true) { delay(1000); } }
}

// =========================================================
//  loop() : libre (inputs/táctil/BLE). Ceder CPU.
// =========================================================
void loop() { vTaskDelay(pdMS_TO_TICKS(100)); }
