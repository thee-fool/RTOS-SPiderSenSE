/*******************************************************************************
 * ESP32-CAM TRIGGER SLAVE — "Spider-Bot Eyes"
 *
 * Module 3 — Slave Side (ESP32-CAM)  [UART-ONLY + MTMN FACE DETECTION]
 *
 * Uses the ESP32-CAM's BUILT-IN MTMN neural network face detector via
 * fd_forward.h — NO external libraries, NO WiFi, NO esp-who needed.
 *
 * ─── IMPORTANT: BOARD PACKAGE VERSION ───────────────────────────────────────
 * fd_forward.h ONLY exists in ESP32 Arduino core v1.0.4 – v1.0.6.
 * It was REMOVED in v2.x.
 *
 * In Arduino IDE:
 *   Tools → Board → Boards Manager → search "esp32 by Espressif"
 *   Install version 1.0.6  (NOT the latest 2.x)
 *
 * If you are stuck on v2.x and cannot downgrade, see the #else block at the
 * bottom — it falls back to the improved skin-tone detector with a warning.
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * Flow on [CAPTURE] received:
 *   1. Capture JPEG frame
 *   2. Convert JPEG → RGB888 (required by MTMN)
 *   3. Run face_detect() — returns bounding boxes
 *   4. Send [THREAT]level=N,faces=M[/THREAT]
 *   5. Send JPEG as [FRAME_S]<4B len><bytes>[FRAME_E]
 *
 * Hardware:
 *   - AI-Thinker ESP32-CAM
 *   - UART0 TX (GPIO1)  →  Master ESP32 UART2 RX (GPIO16)
 *   - UART0 RX (GPIO3)  ←  Master ESP32 UART2 TX (GPIO17)
 *   - Common GND
 *
 * Board:     "AI Thinker ESP32-CAM"
 * Partition: "Huge APP (3MB No OTA / 1MB SPIFFS)"
 * Core ver:  1.0.6  (required for fd_forward.h)
 ******************************************************************************/

#include "esp_camera.h"

// ── Check if MTMN is available (core ≤ 1.0.6) ──────────────────────────────
// The fd_forward.h header lives in the esp-face SDK component that was
// bundled with the older Arduino ESP32 core. We detect availability by
// checking the ESP_ARDUINO_VERSION macro introduced in 2.0.0.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 2
// Core 2.x — fd_forward.h removed. Fall back to improved skin detector.
#define USE_MTMN 0
#warning                                                                       \
    "ESP32 core v2.x detected — fd_forward.h unavailable. Using skin-tone fallback."
#warning "For MTMN neural-net face detection, downgrade to ESP32 core v1.0.6."
#else
// Core 1.x — MTMN available
#define USE_MTMN 1
#include "fd_forward.h"
#endif

#include <HardwareSerial.h>

// ─────────────────────────── AI-THINKER PIN MAP ─────────────────────────────
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ─────────────────────────── CONFIGURATION ──────────────────────────────────
#define UART_BAUD 460800
#define ONBOARD_LED_PIN 33
#define FLASH_LED_PIN 4

static const uint8_t FRAME_START[] = "[FRAME_S]";
static const uint8_t FRAME_END[] = "[FRAME_E]";
#define MARKER_LEN 9

// ─────────────────────────── MTMN CONFIG ─────────────────────────────────────
#if USE_MTMN
// FAST mode: ~300–600ms per frame, good accuracy at QVGA
// NORMAL mode: ~800ms–1.2s, higher accuracy — try if FAST misses faces
static mtmn_config_t mtmn_config;

void initMTMN() {
  memset(&mtmn_config, 0, sizeof(mtmn_config));
  mtmn_config.type = FAST; // or NORMAL for better accuracy
  mtmn_config.min_face =
      80; // min face size in pixels — lower catches smaller faces
  mtmn_config.pyramid = 0.707f;
  mtmn_config.pyramid_times = 4;

  // P-net (proposal) — coarser scan, filters most of the frame
  mtmn_config.p_threshold.score = 0.6f;
  mtmn_config.p_threshold.nms = 0.7f;
  mtmn_config.p_threshold.candidate_number = 20;

  // R-net (refine)
  mtmn_config.r_threshold.score = 0.7f;
  mtmn_config.r_threshold.nms = 0.7f;
  mtmn_config.r_threshold.candidate_number = 10;

  // O-net (output) — fine detection, 1 candidate = take best result
  mtmn_config.o_threshold.score = 0.7f;
  mtmn_config.o_threshold.nms = 0.4f;
  mtmn_config.o_threshold.candidate_number = 1;
}
#endif

// ─────────────────────────── FALLBACK SKIN DETECTOR ──────────────────────────
// Used when MTMN is unavailable (core v2.x).
// Improved over the original — tighter thresholds, better filtering.
#if !USE_MTMN

#define FRAME_W 320
#define FRAME_H 240
#define GRID_CELL_W 10
#define GRID_CELL_H 10
#define GRID_COLS (FRAME_W / GRID_CELL_W)
#define GRID_ROWS (FRAME_H / GRID_CELL_H)
#define MAX_FACE_BLOBS 5
#define MIN_BLOB_GRID_CELLS 12
#define MIN_BLOB_DIM 3
#define MIN_BLOB_DENSITY 0.35f
#define MIN_ASPECT 0.7f
#define MAX_ASPECT 1.8f
#define SKIN_CELL_THRESHOLD 0.55f

typedef struct {
  int minR, minC, maxR, maxC, cellCount, pixelCount;
} BlobInfo_t;
static uint8_t skinGrid[GRID_ROWS][GRID_COLS];
static uint8_t visited[GRID_ROWS][GRID_COLS];

inline bool isSkinTone(uint16_t rgb565) {
  uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
  uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
  uint8_t b = ((rgb565) & 0x1F) << 3;
  int lum = (r * 2 + g * 5 + b) >> 3;
  if (lum < 60 || lum > 220)
    return false;
  if (r < 95 || g < 50 || b < 25)
    return false;
  if (r <= g || r <= b)
    return false;
  if ((r - g) < 20 || (r - b) < 30)
    return false;
  if (g <= b || g < (r * 2 / 5))
    return false;
  if ((r - b) < (r / 4))
    return false;
  return true;
}

void floodFill(int sr, int sc, BlobInfo_t *blob) {
  struct {
    int r, c;
  } stack[GRID_ROWS * GRID_COLS];
  int top = 0;
  stack[top++] = {sr, sc};
  visited[sr][sc] = 1;
  blob->minR = blob->maxR = sr;
  blob->minC = blob->maxC = sc;
  blob->cellCount = blob->pixelCount = 0;
  const int dr[] = {-1, 1, 0, 0}, dc[] = {0, 0, -1, 1};
  while (top > 0) {
    auto c = stack[--top];
    int r = c.r, cc = c.c;
    blob->cellCount++;
    blob->pixelCount += skinGrid[r][cc];
    if (r < blob->minR)
      blob->minR = r;
    if (r > blob->maxR)
      blob->maxR = r;
    if (cc < blob->minC)
      blob->minC = cc;
    if (cc > blob->maxC)
      blob->maxC = cc;
    for (int d = 0; d < 4; d++) {
      int nr = r + dr[d], nc = cc + dc[d];
      if (nr >= 0 && nr < GRID_ROWS && nc >= 0 && nc < GRID_COLS &&
          !visited[nr][nc] && skinGrid[nr][nc] > 0) {
        visited[nr][nc] = 1;
        if (top < GRID_ROWS * GRID_COLS)
          stack[top++] = {nr, nc};
      }
    }
  }
}

int detectFacesSkin(uint16_t *pixels, int w, int h) {
  memset(skinGrid, 0, sizeof(skinGrid));
  int spc = (GRID_CELL_W / 2) * (GRID_CELL_H / 2);
  int thr = (int)(spc * SKIN_CELL_THRESHOLD);
  for (int gy = 0; gy < GRID_ROWS; gy++)
    for (int gx = 0; gx < GRID_COLS; gx++) {
      int cnt = 0;
      for (int dy = 0; dy < GRID_CELL_H; dy += 2)
        for (int dx = 0; dx < GRID_CELL_W; dx += 2) {
          int px = gx * GRID_CELL_W + dx, py = gy * GRID_CELL_H + dy;
          if (px < w && py < h && isSkinTone(pixels[py * w + px]))
            cnt++;
        }
      skinGrid[gy][gx] = (cnt >= thr) ? cnt : 0;
    }
  memset(visited, 0, sizeof(visited));
  BlobInfo_t blobs[MAX_FACE_BLOBS];
  int bc = 0;
  for (int r = 0; r < GRID_ROWS && bc < MAX_FACE_BLOBS; r++)
    for (int c = 0; c < GRID_COLS && bc < MAX_FACE_BLOBS; c++) {
      if (skinGrid[r][c] > 0 && !visited[r][c]) {
        BlobInfo_t b;
        floodFill(r, c, &b);
        if (b.cellCount < MIN_BLOB_GRID_CELLS)
          continue;
        int bh = b.maxR - b.minR + 1, bw = b.maxC - b.minC + 1;
        if (bh < MIN_BLOB_DIM || bw < MIN_BLOB_DIM)
          continue;
        float asp = (float)bh / (float)bw;
        if (asp < MIN_ASPECT || asp > MAX_ASPECT)
          continue;
        if ((float)b.cellCount / (float)(bh * bw) < MIN_BLOB_DENSITY)
          continue;
        blobs[bc++] = b;
      }
    }
  return bc;
}
#endif // !USE_MTMN

// ─────────────────────────── RTOS OBJECTS ────────────────────────────────────
SemaphoreHandle_t captureRequestSem;

// ═══════════════════════════════════════════════════════════════════════════════
// CAMERA INIT
// ═══════════════════════════════════════════════════════════════════════════════
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  // Core 1.0.6 uses pin_sscb_sda / pin_sscb_scl (older naming)
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;

#if USE_MTMN
  // MTMN needs JPEG input — fmt2rgb888 converts it internally
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 12;
#else
  // Skin detector needs raw RGB565
  config.pixel_format = PIXFORMAT_RGB565;
  config.jpeg_quality = 12;
#endif

  config.frame_size = FRAMESIZE_QVGA; // 320×240 — required by MTMN
  // grab_mode and fb_location were added in core 2.x — not available in 1.0.6
  // fb_count=2 with PSRAM gives double-buffering on 1.0.6
  if (psramFound()) {
    config.fb_count = 2;
  } else {
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK)
    return false;

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_gainceiling(s, (gainceiling_t)4);
    // Horizontal flip — uncomment if your camera is mounted upside-down
    // s->set_hmirror(s, 1);
    // s->set_vflip(s, 1);
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// SEND FRAMED JPEG over UART0
// ═══════════════════════════════════════════════════════════════════════════════
void sendFramedJpeg(const uint8_t *data, size_t len) {
  Serial.write(FRAME_START, MARKER_LEN);
  uint8_t lb[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16),
                   (uint8_t)(len >> 8), (uint8_t)(len)};
  Serial.write(lb, 4);
  size_t sent = 0;
  while (sent < len) {
    size_t chunk = min((size_t)1024, len - sent);
    Serial.write(data + sent, chunk);
    Serial.flush();
    sent += chunk;
  }
  Serial.write(FRAME_END, MARKER_LEN);
  Serial.flush();
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 1: Task_UartListen — Core 0 | Priority 5
// ═══════════════════════════════════════════════════════════════════════════════
void Task_UartListen(void *pvParameters) {
  (void)pvParameters;
  while (true) {
    if (Serial.available()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line == "[CAPTURE]") {
        xSemaphoreGive(captureRequestSem);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 2: Task_CaptureAndDetect — Core 1 | Priority 5
//
// MTMN path (core 1.x):
//   Capture JPEG → fmt2rgb888 → face_detect() → send threat → send JPEG
//
// Skin fallback (core 2.x):
//   Capture RGB565 → detectFacesSkin() → fmt2jpg → send threat → send JPEG
// ═══════════════════════════════════════════════════════════════════════════════
void Task_CaptureAndDetect(void *pvParameters) {
  (void)pvParameters;

  // Let auto-exposure settle
  for (int i = 0; i < 5; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb)
      esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(80));
  }

  while (true) {
    if (xSemaphoreTake(captureRequestSem, portMAX_DELAY) != pdTRUE)
      continue;

    uint32_t t0 = millis();
    digitalWrite(FLASH_LED_PIN, HIGH);

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      digitalWrite(FLASH_LED_PIN, LOW);
      Serial.println("[THREAT]level=-1,faces=0[/THREAT]");
      Serial.flush();
      continue;
    }

    int faceCount = 0;

// ──────────────────────────────────────────────────────────────────────────────
#if USE_MTMN
    // ── MTMN Neural Network Detection
    // ────────────────────────────────────────────
    //
    // face_detect() needs RGB888. We allocate a dl_matrix3du for it.
    // Memory usage: 320*240*3 = 230 400 bytes — needs PSRAM!
    // If no PSRAM the board will crash here. All AI-Thinker boards have 4MB
    // PSRAM.

    dl_matrix3du_t *rgb_matrix =
        dl_matrix3du_alloc(1, fb->width, fb->height, 3);

    if (rgb_matrix) {
      // Convert JPEG → RGB888 into the matrix buffer
      bool converted =
          fmt2rgb888(fb->buf, fb->len, fb->format, rgb_matrix->item);

      if (converted) {
        uint32_t det_t = millis();
        box_array_t *boxes = face_detect(rgb_matrix, &mtmn_config);

        if (boxes) {
          faceCount = boxes->len; // number of detected face bounding boxes

          // Free box sub-arrays (required — otherwise memory leak)
          dl_lib_free(boxes->score);
          dl_lib_free(boxes->box);
          if (boxes->landmark)
            dl_lib_free(boxes->landmark);
          dl_lib_free(boxes);
        }
        // boxes == NULL → no faces detected, faceCount stays 0

        Serial.printf("[DEBUG_SLAVE] MTMN: %d faces in %lu ms\n", faceCount,
                      millis() - det_t);
      } else {
        Serial.println("[DEBUG_SLAVE] fmt2rgb888 failed");
      }

      dl_matrix3du_free(rgb_matrix);
    } else {
      Serial.println(
          "[DEBUG_SLAVE] dl_matrix3du_alloc failed — not enough PSRAM?");
    }

    // The captured frame IS the JPEG — send it directly, no re-encode needed
    int threat = (faceCount == 0)   ? 0
                 : (faceCount == 1) ? 2
                 : (faceCount == 2) ? 3
                 : (faceCount == 3) ? 4
                                    : 5;
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "[THREAT]level=%d,faces=%d[/THREAT]", threat,
             faceCount);
    Serial.println(tbuf);
    Serial.flush();

    // Verify JPEG header before sending
    if (fb->len >= 2 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8) {
      sendFramedJpeg(fb->buf, fb->len);
    } else {
      Serial.println("[DEBUG_SLAVE] Bad JPEG header — skipping TX");
    }

    esp_camera_fb_return(fb);

// ──────────────────────────────────────────────────────────────────────────────
#else
    // ── Skin-Tone Fallback (core 2.x)
    // ────────────────────────────────────────────

    if (fb->format == PIXFORMAT_RGB565) {
      faceCount = detectFacesSkin((uint16_t *)fb->buf, fb->width, fb->height);
    }

    int threat = (faceCount == 0)   ? 0
                 : (faceCount == 1) ? 2
                 : (faceCount == 2) ? 3
                 : (faceCount == 3) ? 4
                                    : 5;
    char tbuf[64];
    snprintf(tbuf, sizeof(tbuf), "[THREAT]level=%d,faces=%d[/THREAT]", threat,
             faceCount);
    Serial.println(tbuf);
    Serial.flush();

    // RGB565 → JPEG for transmission
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    bool ok = fmt2jpg(fb->buf, fb->len, fb->width, fb->height, fb->format, 20,
                      &jpeg_buf, &jpeg_len);
    esp_camera_fb_return(fb);

    if (ok && jpeg_buf && jpeg_len > 0) {
      sendFramedJpeg(jpeg_buf, jpeg_len);
      free(jpeg_buf);
    } else {
      Serial.println("[DEBUG_SLAVE] fmt2jpg failed");
      if (jpeg_buf)
        free(jpeg_buf);
    }

#endif
    // ──────────────────────────────────────────────────────────────────────────────

    digitalWrite(FLASH_LED_PIN, LOW);
    Serial.printf("[DEBUG_SLAVE] Total: %lu ms | Threat: %d | Faces: %d\n",
                  millis() - t0, threat, faceCount);
    Serial.flush();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 3: Task_Debug — LED heartbeat
// ═══════════════════════════════════════════════════════════════════════════════
void Task_Debug(void *pvParameters) {
  (void)pvParameters;
  bool s = false;
  while (true) {
    s = !s;
    digitalWrite(ONBOARD_LED_PIN, s ? LOW : HIGH); // active-low
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(UART_BAUD);
  delay(500);

  pinMode(ONBOARD_LED_PIN, OUTPUT);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(ONBOARD_LED_PIN, HIGH);
  digitalWrite(FLASH_LED_PIN, LOW);

#if USE_MTMN
  Serial.println("[SLAVE] Face detection: MTMN neural network (fd_forward.h)");
  initMTMN();
#else
  Serial.println("[SLAVE] Face detection: skin-tone blob fallback (core 2.x)");
  Serial.println("[SLAVE] Downgrade to ESP32 core 1.0.6 for MTMN.");
#endif

  if (!initCamera()) {
    // rapid blink = camera init fail
    while (true) {
      digitalWrite(ONBOARD_LED_PIN, LOW);
      delay(80);
      digitalWrite(ONBOARD_LED_PIN, HIGH);
      delay(80);
    }
  }

  captureRequestSem = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(Task_UartListen, "UartLsn", 4096, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(Task_CaptureAndDetect, "CapDet", 16384, NULL, 5, NULL,
                          1);
  xTaskCreatePinnedToCore(Task_Debug, "Debug", 2048, NULL, 1, NULL, 0);

  Serial.println("[SLAVE] Spider-Bot Eyes online. Awaiting [CAPTURE]...");
}

void loop() { vTaskDelay(portMAX_DELAY); }
