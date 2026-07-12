/*******************************************************************************
 * ESP32 TRIGGER MASTER — "Peter's Brain + Tingle"
 *
 * Module 3 — Master Side (ESP32 with INMP441)  [UART-ONLY EDITION]
 *
 * Flow:
 *   1. INMP441 detects anomaly (clap) → OR → button press → OR → PC sends
 * [CAPTURE]
 *   2. Sends [CAPTURE] to ESP32-CAM via UART2
 *   3. ESP32-CAM sends back:
 *        a) Text line: [THREAT]level=N,faces=M[/THREAT]
 *        b) Binary:    [FRAME_S]<4-byte len><JPEG bytes>[FRAME_E]
 *   4. Master relays BOTH to PC over Serial0 (USB)
 *
 * Hardware:
 *   - ESP32 Dev Module
 *   - INMP441 I2S Mic: SCK→GPIO26, WS→GPIO25, SD→GPIO33, L/R→GND
 *   - UART2 to ESP32-CAM:
 *       GPIO16 (RX2) ←  ESP32-CAM GPIO1  (UART0 TX)
 *       GPIO17 (TX2) →  ESP32-CAM GPIO3  (UART0 RX)
 *   - Manual trigger button: GPIO4 → GND
 *
 * FreeRTOS Tasks:
 *   Task_SpiderSense_Listen → Core 1, Priority 5  (I2S audio)
 *   Task_Karen_Assess       → Core 1, Priority 4  (FFT + anomaly)
 *   Task_WebTether          → Core 0, Priority 4  (PC serial I/O)
 *   Task_CamCommand         → Core 0, Priority 5  (send [CAPTURE] to CAM)
 *   Task_CamReceive         → Core 0, Priority 4  (relay THREAT + JPEG to PC)
 ******************************************************************************/

#include <HardwareSerial.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>

// ─────────────────────────── INMP441 PIN DEFINITIONS ────────────────────────
#define I2S_WS_PIN 25
#define I2S_SCK_PIN 26
#define I2S_SD_PIN 33

// ─────────────────────────── UART2 TO ESP32-CAM ─────────────────────────────
#define UART2_RX_PIN 16   // ← ESP32-CAM UART0 TX (GPIO1)
#define UART2_TX_PIN 17   // → ESP32-CAM UART0 RX (GPIO3)
#define UART2_BAUD 460800 // Must match slave UART_BAUD

// ─────────────────────────── PC SERIAL ──────────────────────────────────────
#define PC_BAUD 921600 // USB to PC — high speed for JPEG relay

// ─────────────────────────── MANUAL TRIGGER ──────────────────────────────────
#define BUTTON_PIN 4
#define DEBOUNCE_MS 300

// ─────────────────────────── AUDIO CONFIG ────────────────────────────────────
#define SAMPLE_RATE_HZ 16000
#define FFT_SAMPLES 256
#define FFT_BIN_COUNT (FFT_SAMPLES / 2)

#define TINGLE_BIN_LOW 64
#define TINGLE_BIN_HIGH 96
#define TINGLE_THRESHOLD 8000.0
#define TINGLE_COOLDOWN_MS 3000

#define I2S_PORT I2S_NUM_0
#define I2S_DMA_BUF_LEN FFT_SAMPLES
#define I2S_DMA_BUF_COUNT 8

// ─────────────────────────── FRAME PROTOCOL ──────────────────────────────────
static const uint8_t FRAME_START[] = "[FRAME_S]";
static const uint8_t FRAME_END[] = "[FRAME_E]";
#define MARKER_LEN 9

// Max JPEG we expect from QVGA @ quality 20 — typically 8–15KB, 32KB is safe
#define MAX_JPEG_SIZE 32768

// ─────────────────────────── RTOS OBJECTS ────────────────────────────────────
QueueHandle_t webCartridgeAudio;
QueueHandle_t webCartridgeFFT;
SemaphoreHandle_t tingleSemaphore;
SemaphoreHandle_t captureSemaphore;

// ─────────────────────────── DATA STRUCT ─────────────────────────────────────
typedef struct {
  uint16_t bins[FFT_BIN_COUNT];
} FFTResult_t;

// ─────────────────────────── GLOBAL STATE ────────────────────────────────────
static volatile unsigned long lastTriggerTime = 0;
static volatile unsigned long lastButtonTime = 0;
static volatile bool waitingForResp = false;
static volatile unsigned long captureSentTime = 0;

// Receive buffer for JPEG coming in from CAM over UART2.
// Static — lives in heap, not stack.
static uint8_t jpegRxBuf[MAX_JPEG_SIZE];

// ═══════════════════════════════════════════════════════════════════════════════
// GPIO ISR — button
// ═══════════════════════════════════════════════════════════════════════════════
void IRAM_ATTR buttonISR() {
  unsigned long now = millis();
  if (now - lastButtonTime > DEBOUNCE_MS) {
    lastButtonTime = now;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(captureSemaphore, &woken);
    if (woken)
      portYIELD_FROM_ISR();
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// I2S INIT
// ═══════════════════════════════════════════════════════════════════════════════
void initI2S() {
  i2s_config_t cfg = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
                      .sample_rate = SAMPLE_RATE_HZ,
                      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
                      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
                      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
                      .dma_buf_count = I2S_DMA_BUF_COUNT,
                      .dma_buf_len = I2S_DMA_BUF_LEN,
                      .use_apll = false,
                      .tx_desc_auto_clear = false,
                      .fixed_mclk = 0};
  i2s_pin_config_t pins = {.bck_io_num = I2S_SCK_PIN,
                           .ws_io_num = I2S_WS_PIN,
                           .data_out_num = I2S_PIN_NO_CHANGE,
                           .data_in_num = I2S_SD_PIN};
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
}

// ═══════════════════════════════════════════════════════════════════════════════
// HELPER: Read exact N bytes from UART2 with timeout
// ═══════════════════════════════════════════════════════════════════════════════
bool uart2ReadExact(uint8_t *buf, size_t count, uint32_t timeoutMs) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < count) {
    if (millis() - start > timeoutMs)
      return false;
    if (Serial2.available()) {
      buf[got++] = Serial2.read();
    } else {
      vTaskDelay(1);
    }
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 1: Task_SpiderSense_Listen — Core 1 | Priority 5
// ═══════════════════════════════════════════════════════════════════════════════
void Task_SpiderSense_Listen(void *pvParameters) {
  (void)pvParameters;
  int32_t buf[FFT_SAMPLES];
  size_t bytesIn = 0;
  while (true) {
    if (i2s_read(I2S_PORT, buf, sizeof(buf), &bytesIn, portMAX_DELAY) ==
        ESP_OK) {
      int n = bytesIn / sizeof(int32_t);
      for (int i = 0; i < n; i++) {
        int16_t s = buf[i] >> 14;
        xQueueSend(webCartridgeAudio, &s, 0);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 2: Task_Karen_Assess — Core 1 | Priority 4
// ═══════════════════════════════════════════════════════════════════════════════
void Task_Karen_Assess(void *pvParameters) {
  (void)pvParameters;
  double vReal[FFT_SAMPLES], vImag[FFT_SAMPLES];
  ArduinoFFT<double> FFT(vReal, vImag, FFT_SAMPLES, (double)SAMPLE_RATE_HZ);

  while (true) {
    for (int i = 0; i < FFT_SAMPLES; i++) {
      int16_t s;
      xQueueReceive(webCartridgeAudio, &s, portMAX_DELAY);
      vReal[i] = (double)s;
      vImag[i] = 0.0;
    }

    FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    FFTResult_t result;
    double tingleEnergy = 0.0;
    result.bins[0] = 0;
    for (int i = 1; i < FFT_BIN_COUNT; i++) {
      result.bins[i] = (uint16_t)min(vReal[i] / 10.0, 65535.0);
      if (i >= TINGLE_BIN_LOW && i <= TINGLE_BIN_HIGH)
        tingleEnergy += vReal[i];
    }

    xQueueSend(webCartridgeFFT, &result, 0);

    unsigned long now = millis();
    if (tingleEnergy > TINGLE_THRESHOLD &&
        (now - lastTriggerTime > TINGLE_COOLDOWN_MS)) {
      lastTriggerTime = now;
      xSemaphoreGive(tingleSemaphore);
      xSemaphoreGive(captureSemaphore);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 3: Task_WebTether — Core 0 | Priority 4
// Forwards FFT data + tingle events to PC. Reads [CAPTURE] commands from PC.
// ═══════════════════════════════════════════════════════════════════════════════
void Task_WebTether(void *pvParameters) {
  (void)pvParameters;

  while (true) {
    // Tingle notification to PC
    if (xSemaphoreTake(tingleSemaphore, 0) == pdTRUE) {
      Serial.println("[TINGLE]");
    }

    // FFT data — only send when not in the middle of relaying a frame
    FFTResult_t fftData;
    if (xQueueReceive(webCartridgeFFT, &fftData, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (!waitingForResp) {
        Serial.print("[FFT]");
        for (int i = 0; i < FFT_BIN_COUNT; i++) {
          Serial.print(fftData.bins[i]);
          if (i < FFT_BIN_COUNT - 1)
            Serial.print(",");
        }
        Serial.println("[/FFT]");
      }
    }

    // Accept [CAPTURE] commands typed from PC serial monitor / Python HUD
    if (Serial.available()) {
      String cmd = Serial.readStringUntil('\n');
      cmd.trim();
      if (cmd == "[CAPTURE]") {
        xSemaphoreGive(captureSemaphore);
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 4: Task_CamCommand — Core 0 | Priority 5
// Waits for captureSemaphore, sends [CAPTURE] to ESP32-CAM over UART2.
// ═══════════════════════════════════════════════════════════════════════════════
void Task_CamCommand(void *pvParameters) {
  (void)pvParameters;

  while (true) {
    // Deadlock guard: if CAM doesn't respond in 15s, reset state
    if (waitingForResp && (millis() - captureSentTime > 15000)) {
      Serial.println("[CAM_CMD] Response timeout — resetting.");
      waitingForResp = false;
    }

    if (xSemaphoreTake(captureSemaphore, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!waitingForResp) {
        waitingForResp = true;
        captureSentTime = millis();
        Serial2.println("[CAPTURE]");
        Serial2.flush();
        Serial.println("[CAM_CMD] [CAPTURE] sent to Spider-Bot Eyes.");
      } else {
        Serial.println("[CAM_CMD] Busy — trigger ignored.");
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TASK 5: Task_CamReceive — Core 0 | Priority 4
//
// Reads from UART2 (ESP32-CAM). Two types of data arrive:
//
//   A) Text line  →  [THREAT]level=N,faces=M[/THREAT]
//                    [DEBUG_SLAVE] ...
//
//   B) Binary frame → [FRAME_S]<4-byte len><JPEG bytes>[FRAME_E]
//
// Strategy: accumulate bytes into a small look-ahead buffer.
// When we see [FRAME_S], switch to binary receive mode.
// Otherwise, process as text lines.
//
// The JPEG is read directly into jpegRxBuf and then forwarded to PC
// over Serial0 using the same [FRAME_S]...[FRAME_E] protocol.
// The PC Python script already knows how to handle it.
// ═══════════════════════════════════════════════════════════════════════════════
void Task_CamReceive(void *pvParameters) {
  (void)pvParameters;

  // Rolling look-ahead for marker detection (only needs MARKER_LEN bytes)
  uint8_t lookAhead[MARKER_LEN];
  int laLen = 0; // bytes currently in look-ahead
  String textLine = "";

  while (true) {

    if (!Serial2.available()) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    uint8_t b = Serial2.read();

    // ── Check if we've just completed the [FRAME_S] marker ──────────────
    // Append to look-ahead, slide window
    if (laLen < MARKER_LEN) {
      lookAhead[laLen++] = b;
    } else {
      memmove(lookAhead, lookAhead + 1, MARKER_LEN - 1);
      lookAhead[MARKER_LEN - 1] = b;
    }

    bool markerFound = (laLen == MARKER_LEN &&
                        memcmp(lookAhead, FRAME_START, MARKER_LEN) == 0);

    if (markerFound) {
      // ── BINARY FRAME MODE ──────────────────────────────────────────────
      laLen = 0;     // Reset look-ahead
      textLine = ""; // Discard any partial text line

      // 1. Read 4-byte length
      uint8_t lenBytes[4];
      if (!uart2ReadExact(lenBytes, 4, 3000)) {
        Serial.println("[CAM_RX] Timeout reading frame length");
        waitingForResp = false;
        continue;
      }

      uint32_t jpegLen = ((uint32_t)lenBytes[0] << 24) |
                         ((uint32_t)lenBytes[1] << 16) |
                         ((uint32_t)lenBytes[2] << 8) | ((uint32_t)lenBytes[3]);

      if (jpegLen == 0 || jpegLen > MAX_JPEG_SIZE) {
        Serial.printf("[CAM_RX] Bad JPEG length: %lu\n", jpegLen);
        waitingForResp = false;
        continue;
      }

      // 2. Read JPEG data into buffer
      if (!uart2ReadExact(jpegRxBuf, jpegLen, 8000)) {
        Serial.printf("[CAM_RX] Timeout reading JPEG data (%lu bytes)\n",
                      jpegLen);
        waitingForResp = false;
        continue;
      }

      // 3. Read + verify end marker
      uint8_t endMarker[MARKER_LEN];
      if (!uart2ReadExact(endMarker, MARKER_LEN, 2000)) {
        Serial.println("[CAM_RX] Timeout reading end marker");
        waitingForResp = false;
        continue;
      }

      if (memcmp(endMarker, FRAME_END, MARKER_LEN) != 0) {
        Serial.println("[CAM_RX] End marker mismatch — frame dropped");
        waitingForResp = false;
        continue;
      }

      // 4. Relay complete JPEG to PC over Serial0
      //    Pause FFT printing while we do this (Task_WebTether checks
      //    waitingForResp)
      Serial.write(FRAME_START, MARKER_LEN);
      Serial.write(lenBytes, 4);

      const size_t CHUNK = 1024;
      size_t sent = 0;
      while (sent < jpegLen) {
        size_t toSend = min(CHUNK, (size_t)(jpegLen - sent));
        Serial.write(jpegRxBuf + sent, toSend);
        Serial.flush();
        sent += toSend;
      }

      Serial.write(FRAME_END, MARKER_LEN);
      Serial.flush();

      Serial.printf("\n[CAM_RX] Relayed %lu bytes to PC.\n", jpegLen);
      waitingForResp = false;

    } else {
      // ── TEXT LINE MODE ─────────────────────────────────────────────────
      if (b == '\n') {
        textLine.trim();
        if (textLine.length() > 0) {

          if (textLine.startsWith("[THREAT]") &&
              textLine.endsWith("[/THREAT]")) {
            // Forward directly to PC — Python HUD parses [CAM_THREAT] format
            // but we also want to convert to match what HUD expects
            int lvlStart = textLine.indexOf("level=") + 6;
            int facStart = textLine.indexOf("faces=") + 6;
            int commaPos = textLine.indexOf(",", lvlStart);
            int endPos = textLine.indexOf("[/THREAT]");

            int level = textLine.substring(lvlStart, commaPos).toInt();
            int faces = textLine.substring(facStart, endPos).toInt();

            // Emit in the format the Python HUD already parses
            Serial.printf("[CAM_THREAT] level=%d faces=%d\n", level, faces);

          } else if (textLine.startsWith("[DEBUG_SLAVE]")) {
            Serial.printf("[CAM_DBG] %s\n", textLine.c_str());
          } else {
            Serial.printf("[CAM_RAW] %s\n", textLine.c_str());
          }
        }
        textLine = "";
      } else if (b != '\r') {
        textLine += (char)b;
        // Safety: cap line length to avoid unbounded growth
        if (textLine.length() > 256)
          textLine = "";
      }
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
  // USB Serial to PC
  Serial.begin(PC_BAUD);
  delay(500);

  Serial.println("═══════════════════════════════════════════════════");
  Serial.println("  SPIDER-SENSE SENTRY — TRIGGER MASTER (UART-ONLY)");
  Serial.println("═══════════════════════════════════════════════════");

  initI2S();

  // UART2 to ESP32-CAM — large RX buffer so incoming JPEG doesn't overrun
  // while Task_CamReceive is being scheduled
  Serial2.setRxBufferSize(MAX_JPEG_SIZE + 256);
  Serial2.begin(UART2_BAUD, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
  Serial.printf("[UART2] CAM link at %d baud (RX:GPIO%d TX:GPIO%d)\n",
                UART2_BAUD, UART2_RX_PIN, UART2_TX_PIN);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);
  Serial.printf("[BTN]  Manual trigger on GPIO%d\n", BUTTON_PIN);

  // RTOS objects
  webCartridgeAudio = xQueueCreate(FFT_SAMPLES * 2, sizeof(int16_t));
  webCartridgeFFT = xQueueCreate(4, sizeof(FFTResult_t));
  tingleSemaphore = xSemaphoreCreateBinary();
  captureSemaphore = xSemaphoreCreateBinary();

  xTaskCreatePinnedToCore(Task_SpiderSense_Listen, "Listen", 8192, NULL, 5,
                          NULL, 1);
  xTaskCreatePinnedToCore(Task_Karen_Assess, "Assess", 8192, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(Task_WebTether, "Tether", 4096, NULL, 4, NULL, 0);
  xTaskCreatePinnedToCore(Task_CamCommand, "CamCmd", 4096, NULL, 5, NULL, 0);
  xTaskCreatePinnedToCore(Task_CamReceive, "CamRcv", 8192, NULL, 4, NULL, 0);

  Serial.println("[MASTER] All tasks live. Waiting for triggers...\n");
}

void loop() { vTaskDelay(portMAX_DELAY); }
