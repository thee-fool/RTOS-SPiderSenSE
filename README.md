# 🕸 THE SPIDER-SENSE SENTRY

> **RTOS Project (Theme: Spider-Man Multiverse)**: RTOS Task Prioritization, ESP32-CAM MTMN Face Detection, UART IPC, Audio FFT Processing, Real-Time Python HUD.

The Multiverse has fractured! To keep the neighborhood safe, Peter Parker has built a hardwired, localized defense system using scavenged Stark Tech.

- **ESP32 (Master — "Peter Tingle")**: Reads ambient audio via an INMP441 I2S microphone, constantly sampling the environment with FFT for the specific frequency anomaly of an intruder (e.g. a loud clap or snap).
- **ESP32-CAM (Slave — "Spider-Bot Eyes")**: Runs onboard MTMN face detection and captures imagery when triggered by the Master entirely independent of WiFi/Web Servers.
- **Laptop (PC — "K.A.R.E.N. HUD")**: Runs a Python dashboard with live audio spectrum waveform, threat assessment UI, and real-time relayed images.

When an anomaly is detected, the Master ESP32 sends a trigger command to the ESP32-CAM. The ESP32-CAM captures a frame, detects villains, and relays the photo and threat level back through the Master to your Laptop!

---

## 📁 Project Components

1. **`esp32_trigger_capture/esp32_trigger_master`** (Master ESP32 Code)
2. **`esp32_trigger_capture/esp32cam_trigger_slave`** (Slave ESP32-CAM Code)
3. **`pc_display`** (Python HUD Code)

---

## 🔌 Hardware Wiring & Setup

### 1. INMP441 Microphone → ESP32 Master

| INMP441 Pin | ESP32 Pin | Note |
|-------------|-----------|------|
| SCK / BCLK  | GPIO26    | Clock |
| WS / LRCLK  | GPIO25    | Word Select |
| SD / DOUT   | GPIO33    | Serial Data |
| L/R         | GND       | Tying to GND outputs on Left channel |
| VDD         | 3.3V      | Power |
| GND         | GND       | Ground |


### 2. ESP32 Master ↔ ESP32-CAM Slave (UART2)

Both boards communicate via UART at **460800 baud**. 

| ESP32 Master | | ESP32-CAM Slave |
|--------------|-|-----------------|
| GPIO 17 (TX2)| → | GPIO 3 (RX0) |
| GPIO 16 (RX2)| ← | GPIO 1 (TX0) |
| **VIN** | - | **VIN** |
| **GND**      | ─ | **GND** |

> **⚠ IMPORTANT:** Both boards **MUST** share a common GND. If they are powered via separate USB cables, connect their GND pins together.

---

## 🛠 Flash Instructions

### 1. ESP32 Master (`esp32_trigger_master.ino`)
1. Install the **ESP32** board package in the Arduino IDE.
2. Install the **arduinoFFT** library (by Enrique Condes) via the Library Manager.
3. Select Board: **"ESP32 Dev Module"**.
4. Flash `esp32_trigger_master.ino` to the generic ESP32.

### 2. ESP32-CAM Slave (`esp32cam_trigger_slave.ino`)
> **🚨 CRITICAL STEP:** You MUST use the **1.0.6 version** of the ESP32 Board core for the MTMN Neural Network Face Detection (`fd_forward.h`) to work. This library was removed in core v2.0.0+. 
> *If you use a newer core, the code will compile but fall back to a much less accurate basic skin-tone blob detector.*
1. In Arduino IDE: Go to `Tools` → `Board` → `Boards Manager`, search for "esp32 by Espressif Systems", and select version **1.0.6** to install.
2. Select Board: **"AI Thinker ESP32-CAM"**.
3. Set Partition Scheme: **"Huge APP (3MB No OTA / 1MB SPIFFS)"**.
4. Flash `esp32cam_trigger_slave.ino` to the ESP32-CAM.

---

## 💻 Running the K.A.R.E.N. Python HUD

Once both microcontrollers are flashed, wired together, and plugged in (The Master ESP32 must be connected via USB to your laptop):

1. Open a terminal and navigate to the PC display folder:
   ```bash
   cd pc_display
   ```
2. Create and activate a Python virtual environment (Recommended):
   - **Windows:**
     ```bash
     python -m venv .venv
     .venv\Scripts\activate
     ```
   - **Mac/Linux:**
     ```bash
     python3 -m venv .venv
     source .venv/bin/activate
     ```
3. Install the required Python dependencies:
   ```bash
   pip install -r requirements.txt
   ```
   *(If you don't have the `requirements.txt` handy, run `pip install pygame opencv-python pyserial numpy`)*
4. Run the display script:
   ```bash
   python KAREN.py --port <YOUR_COM_PORT>
   ```
   *(Replace `<YOUR_COM_PORT>` with the Master ESP32's COM port, e.g., `COM3` on Windows, or `/dev/cu.usbserial-...` on Mac/Linux. If omitted, the script will list available ports and ask you to type one).*

---

## 🎯 Usage: How to Trigger the System

1. **Audio Tingle**: Clap loudly near the INMP441 mic. The FFT running on the Master ESP32 will detect the energy spike and automatically fire a `[CAPTURE]` command.
2. **Manual Button**: Press the hardware push-button on the ESP32 Master's GPIO4.
3. **HUD Button**: Click the on-screen "FIRE CAPTURE" button or press the **SPACEBAR / ENTER** on your laptop keyboard while the Python HUD is in focus.

The system will snap a photo, process it through the AI neural net on the ESP32-CAM, count the faces, and display a threat level alongside the photo directly on your laptop display.

## 📊 Threat Level Scale

| Level | Code       | Faces | Description                          |
|-------|------------|-------|--------------------------------------|
| 0     | SECURE     | 0     | No villains detected                 |
| 1     | GUARDED    | —     | Reserved                             |
| 2     | ELEVATED   | 1     | Single villain spotted               |

