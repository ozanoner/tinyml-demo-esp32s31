# Software Requirements Specification
## tinyml-demo-esp32s31

**Version:** 1.0  
**Project:** TinyML Demo for ESP32-S31 Korvo-1 Devkit  
**SDK:** ESP-IDF v6.0.2

---

## 1. Introduction

### 1.1 Purpose
Build an embedded TinyML application for the **ESP32-S31-Korvo-1 devkit** that:

1. **Voice wake-up** — Uses **WakeNet** (ESP-SR) for always-on wake word detection.
2. **Command detection** — On wake word match, uses **MultiNet** (ESP-SR) to detect the "cheese" command.
3. **Photo capture** — On "cheese" detected, captures an image from the onboard camera.
4. **Object detection** — Runs on-device object detection inference on the captured image using **YOLO26n** (via ESP-DL).
5. **Result display** — Shows the captured image and detection results (bounding boxes, labels, confidence scores) on the devkit's LCD screen.

The application demonstrates a complete voice-triggered vision pipeline running fully on-device, with no cloud dependency.

### 1.2 Operating Environment
- **Hardware:** ESP32-S31-Korvo-1 V1.1 devkit
- **SoC module:** ESP32-S31-WROOM-3 (ESP32-S31 dual-core RISC-V, 16 MB flash, 16 MB PSRAM)
- **SDK:** ESP-IDF v6.0.2
- **Target:** `esp32s31`
- **Toolchain:** Docker-based DevContainer (`espressif/idf:v6.0.2`)
- **Build host:** Linux x86_64 (inside DevContainer)

### 1.3 Glossary
| Term | Definition |
|------|------------|
| WakeNet | ESP-SR voice activation / keyword spotting component |
| ESP-SR | Espressif's Speech Recognition framework |
| MultiNet | ESP-SR speech command recognition model |
| AFE | ESP-SR Audio Front-End (AEC, VAD, noise suppression) |
| ESP-DL | Espressif's deep-learning inference framework |
| YOLO26n | Ultralytics-based object detection model, ported for ESP-DL |
| BSP | Board Support Package — handles all low-level hardware init |
| Korvo-1 | ESP32-S31 devkit with camera, LCD, mic, speaker, and audio codec |
| DevContainer | Docker-based development environment (VSCode Dev Containers) |
| ES8389 | Audio codec on Korvo-1, connected via I2S + I2C |
| DVP | Digital Video Port — parallel camera interface |

---

## 2. Functional Requirements

### REQ-001: ESP-IDF Project Scaffold with DevContainer

Create the ESP-IDF project skeleton using `idf.py create-project` inside the DevContainer, targeting `esp32s31`.

**Acceptance Criteria:**
- [ ] Project created with `idf.py create-project tinyml-demo-esp32s31` inside the DevContainer (IDF v6.0.2)
- [ ] Chip target set to `esp32s31` via `idf.py set-target esp32s31`
- [ ] `.devcontainer/devcontainer.json`, `docker-compose.yml`, and `Dockerfile` configured for `espressif/idf:v6.0.2`
- [ ] Standard ESP-IDF `.gitignore` — covering `build/`, `sdkconfig`, `__pycache__/`, `.pytest_cache/`, `dependencies.lock`, `pytest_embedded_log/`, etc.
- [ ] `main/app_main.cpp` prints a boot banner with chip info, SDK version, and free heap
- [ ] Project compiles successfully: `idf.py build`

---

### REQ-002: BSP Integration and Splash Screen

Add the **ESP32-S31-Korvo-1 BSP** as an IDF component dependency and initialise the board. After boot, display a splash screen for 5 seconds with a demo description message.

The BSP handles all underlying hardware init (audio codec, LCD, touch, camera, SD card, buttons, LED) — no low-level HAL development is needed in this project.

**Acceptance Criteria:**
- [ ] BSP component added via `idf.py add-dependency "espressif/esp32_s31_korvo_1"` from the component registry
- [ ] Individual BSP init functions called per peripheral (`bsp_i2c_init()`, `bsp_audio_init()`, `bsp_display_start()`, camera via BSP's `esp_video`, etc.) — each returns `esp_err_t`, serving as a hardware self-test
- [ ] Any init failure is logged with `ESP_LOGE` and `app_main` returns (per error handling NFR)
- [ ] All peripherals initialised at startup — including camera (OV3660 via `esp_video`), audio, display, touch
- [ ] LVGL and touch initialised via BSP (GT1151 touch controller)
- [ ] Splash screen displayed for 5 s (or until tap) showing the message "TinyML Demo — Voice Triggered Object Detection" in large, readable text — use the full 4.3" LCD effectively
- [ ] Tap anywhere on screen skips the splash immediately and WakeNet listening begins

---

### REQ-003: Application State Display

The application shall display the current operating state on the LCD at all times, so the user always knows what the system is doing. State text shall update instantly with no blank screen gaps between transitions. All display output goes through LVGL (initialised by the BSP in REQ-002).

**Acceptance Criteria:**
- [ ] The following states are displayed on screen as the application progresses:
  - "Listening for wake word..."
  - "Listening for command..."
  - "Taking photo..."
  - "Analysing image..."
  - "No objects detected" (when inference completes with no results)
  - Detection results with bounding box overlay (when objects are found)
- [ ] State transitions are instant — no blank screen between states
- [ ] Splash screen (REQ-002) transitions directly into the first state ("Listening for wake word...")

---

### REQ-004: WakeNet Wake-Word Detection

After the BSP initialisation (REQ-002), integrate **ESP-SR** for always-on wake-word detection using the BSP's audio pipeline. The ESP-SR **Audio Front-End (AFE)** shall be initialised first to handle AEC, noise suppression, and audio buffering; WakeNet then sits on top of AFE.

**Acceptance Criteria:**
- [ ] ESP-SR component (`espressif/esp-sr`) added, supporting ESP32-S31 target (v2.4.6+)
- [ ] ESP-SR AFE initialised for dual-microphone input at 16 kHz
- [ ] WakeNet model loaded and initialised — default wake word: **"Hi,ESP"** (`wn9_hiesp`), configurable via `menuconfig`
- [ ] Continuous audio capture from onboard microphones via the BSP's audio pipeline
- [ ] On wake word match → callback triggers MultiNet listening phase
- [ ] **15-second timeout:** if no command detected within 15 s, fall back to WakeNet listening for wake word
- [ ] Timeout resets / pushes forward during active processing (command detected, capturing, detecting, displaying)
- [ ] Wake word and detection threshold are configurable via `menuconfig` or code constants

---

### REQ-005: MultiNet Command Detection ("cheese")

After WakeNet detects the wake word, activate **ESP-SR MultiNet** to listen for the "cheese" command. The audio pipeline (already initialised by the BSP in REQ-002) is reused — no additional hardware setup needed.

**Acceptance Criteria:**
- [ ] ESP-SR MultiNet model loaded and initialised (targeting ESP32-S31, e.g. `mn7_en`)
- [ ] MultiNet activated on WakeNet wake-word match callback
- [ ] Pre-defined command "cheese" registered in MultiNet's command set
- [ ] On "cheese" detected → callback triggers camera capture
- [ ] On no command within 15 s → fall back to WakeNet (per REQ-004 timeout)
- [ ] MultiNet confidence threshold is configurable via `menuconfig` or code constants

---

### REQ-006: Camera Capture

On the "cheese" command detected by MultiNet, capture a still image from the onboard camera. The camera was already initialised at startup by the BSP in REQ-002 — no additional DVP or sensor configuration needed at capture time.

**Acceptance Criteria:**
- [ ] On "cheese" callback → capture a single still frame from the already-initialised camera
- [ ] Frame buffer allocated in PSRAM
- [ ] Captured frame available in RGB format for both display and detection pipeline
- [ ] Resolution: QVGA (320×240) or configurable
- [ ] Capture completes within acceptable latency (< 500 ms)

---

### REQ-007: YOLO26n Object Detection via ESP-DL

Run object detection on the captured camera frame using **YOLO26n** via **ESP-DL** (v3.3.8+, supports `esp32s31` target). The image is already in PSRAM from REQ-006 — pass it to the inference pipeline.

**Acceptance Criteria:**
- [ ] ESP-DL component added (`espressif/esp-dl` ≥ v3.3.8)
- [ ] YOLO26n model loaded (pre-quantized `.espdl` or custom-trained)
- [ ] Model input tensor populated from camera frame buffer
- [ ] Inference runs on-device (no cloud)
- [ ] Detection results: bounding boxes, class labels, confidence scores
- [ ] Top-N detections (N configurable, default 2) kept for display
- [ ] Inference completes within acceptable latency (< 5 s)

**Fallback:** If YOLO26n is not functional on ESP32-S31, use **ESP-Detection (YOLOv11 pico)** via ESP-DL — a lighter model (0.36M params, ~126 ms on S3) sharing the same ESP-DL runtime.

---

### REQ-008: LCD Display — Image + Detection Results

Display the captured camera frame on the LCD, overlaid with object detection bounding boxes, labels, and confidence scores (from REQ-007). The BSP already initialised display, LVGL, and touch in REQ-002.

**Acceptance Criteria:**
- [ ] Captured camera frame rendered as background image on the LCD
- [ ] Detection bounding boxes drawn as coloured rectangles overlaid on the image
- [ ] Class label + confidence score displayed next to each bounding box in readable text
- [ ] No-detection case: "No objects detected" message shown on screen
- [ ] Results screen auto-dismisses after 10 s and returns to WakeNet listening
- [ ] Tap anywhere dismisses results immediately and returns to WakeNet listening

---

## 3. Non-Functional Requirements

### 3.1 Memory & Storage

**Model selection:**
- Only required models shall be included via `menuconfig` → `Component config` → `ESP Speech Recognition`:
  - **WakeNet:** one wake word model only (not all 50+)
  - **MultiNet:** `mn7_en` only
  - **AFE:** enabled (needed for audio pipeline)
  - **Noise Suppression:** enabled
  - **VAD Model (VADNet):** off unless required by the audio pipeline
  - **Speech Synthesis:** off (not used)
- ESP-DL will only include the YOLO26n `.espdl` model file (or YOLOv11 fallback)

**Model storage (flash):**
- A `model` partition shall be defined in `partitions.csv` with sufficient size for the selected models (typically 6 MB for ESP-SR models)
- Models are flashed to this partition via `idf.py flash` — not embedded in the application binary

**Runtime memory allocation:**
- **SRAM (512 KB internal):** reserved for time-critical audio paths:
  - WakeNet inference buffers
  - Audio pipeline ring buffers
  - RTOS kernel, stacks, Wi-Fi/BT
- **PSRAM (16 MB):** for large, less latency-sensitive data:
  - Camera frame buffer(s) (RGB, e.g. ~230 KB at QVGA)
  - YOLO/ESP-DL tensor arena and weight buffers
  - LVGL framebuffer / draw buffers
  - MultiNet inference buffers (if model is too large for SRAM)
- ESP-DL's static memory planner shall be configured with an appropriate internal SRAM budget; the rest is automatically allocated to PSRAM

### 3.2 Memory Monitoring

After each major software component initialises (BSP, WakeNet, MultiNet, camera, ESP-DL/YOLO), a single-line memory summary shall be printed to UART showing:
- Free SRAM (internal)
- Free PSRAM
- Largest free heap block
- **Warning** if either SRAM or PSRAM usage exceeds 80% of available capacity

### 3.3 Performance Logging

Where applicable, inference / capture timing shall be printed to UART for observability:
- **WakeNet:** time per detection frame (ms)
- **MultiNet:** time per recognition frame (ms)
- **Camera:** frame capture time (ms)
- **YOLO/ESP-DL:** pre-process, inference, and post-process times (ms) — reported individually

### 3.4 Error Handling

- All errors shall be logged with `ESP_LOGE` (not `ESP_LOGW` or `ESP_LOGI`)
- **Init phase errors** (BSP init, WakeNet model load, MultiNet model load, camera init, ESP-DL init): print the error and return from `app_main` (system halts)
- **Operation phase errors** (inference failure, capture failure): skip the current operation cycle and return to WakeNet wake-word listening — do not halt the system

### 3.5 Development Language & Architecture

- **Language:** C++17
- **Architecture:** Modern object-oriented principles with clear separation of concerns
- Each logical part shall be encapsulated in a class (e.g. `AudioPipeline`, `WakeWordDetector`, `CommandDetector`, `Camera`, `ObjectDetector`, `AppStateMachine`)
- **Hardware configurability:** Hardware selection (board type, BSP variant) shall be done through configuration only (`menuconfig` / `sdkconfig`). The application code shall not contain board-specific logic — changing the target hardware must not require source code changes.
- Automated unit / integration tests are **deferred** — not in scope for this project

---

## 4. Edge Cases

| # | Scenario | Current behaviour | Potential issue | Proposed handling |
|---|----------|-------------------|----------------|-------------------|
| EC-001 | "cheese" detected while already processing a previous capture/detect/display cycle | No guard against re-entry — could double-trigger | Both cycles run in parallel, memory corruption, display flicker | Skip the second command — ignore "cheese" if already in a capture/detect/display cycle |
| EC-002 | User taps screen during "Analysing image" or "Taking photo" state | Tap should dismiss or advance | Interrupts processing mid-way, unpredictable state | Ignore — no action taken during active processing states |
| EC-003 | User says wake word ("Hi ESP") while already in an active cycle (not in "Listening for wake word" state) | Audio fed to WakeNet — could trigger a second detection | Double wake word confuses state machine, could launch parallel cycles | Ignore during active cycle — audio data shall not be fed to any model when in an invalid state |
| EC-004 | Camera fails to capture frame (sensor timeout, frame not ready) | No error recovery specified | App stuck waiting for frame | Skip cycle: log error, return to WakeNet listening (per NFR 3.4) |
| EC-005 | YOLO inference exceeds acceptable latency | Hard limit of 5 s specified | Inference hangs, app freezes | Use a timeout (max 5 s). If exceeded, abort inference, log error, and return to WakeNet listening. |
| EC-006 | PSRAM allocation failure at startup (camera buffer, YOLO tensor arena) | App exits with ESP_LOGE | App won't run on boards with insufficient PSRAM | **Deal-breaker** — must be solved before release. Not acceptable as runtime behaviour. |
| EC-007 | Touch controller becomes unresponsive at runtime (not init failure) | Tap gesture never fires — user cannot manually dismiss | App relies solely on timeouts to progress states | Accept degraded behaviour — splash auto-advances at 5 s, results auto-advance at 10 s |

## 5. Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | TBD | Ozan | Initial draft |

---

## 6. References

- [ESP32-S31-Korvo-1 User Guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html)
- [ESP32-S31-Korvo-1 BSP (esp-bsp)](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s31_korvo_1)
- [ESP32-S31-Korvo-1 BSP Compatible Examples](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s31_korvo_1#compatible-bsp-examples)
- [ESP32-S31-Korvo-1 BSP Component Registry](https://components.espressif.com/components/espressif/esp32_s31_korvo_1)
