# Software Requirements Specification
## tinyml-demo-esp32s31

**Version:** 1.1  
**Project:** TinyML Demo for the ESP32-S31 devkits — **ESP32-S31-Korvo-1** and **ESP-Mosaico**  
**SDK:** ESP-IDF 6.2 development line (preview support for ESP32-S31)

> **Status of this document.** REQ-001 … REQ-008 are implemented and run on both devkits. The
> acceptance criteria below are marked against the code in this repository during the
> documentation pass in issue #50, not against the original intent: `- [x]` means the tree meets
> it, `- [ ]` means it does not, with the reason on the line. Where the shipped behaviour differs
> from what was written here, the line says how. **The code is the source of truth**; this
> document records what was agreed and how far it was met.

---

## 1. Introduction

### 1.1 Purpose
Build an embedded TinyML application for the **ESP32-S31 devkits** that:

1. **Voice wake-up** — Uses **WakeNet** (ESP-SR) for always-on wake word detection.
2. **Command detection** — On wake word match, uses **MultiNet** (ESP-SR) to detect the "cheese" command.
3. **Photo capture** — On "cheese" detected, captures an image from the onboard camera.
4. **Object detection** — Runs on-device object detection inference on the captured image using **YOLO11n** (320×320 int8, via the **coco_detect** component on ESP-DL).
5. **Result display** — Shows the captured image and detection results (bounding boxes, labels, confidence scores) on the devkit's LCD screen.

The application demonstrates a complete voice-triggered vision pipeline running fully on-device, with no cloud dependency.

One application builds for either devkit: the devkit preset selects the board support package and
the `devkit::` implementation, and no application code changes between them.

### 1.2 Operating Environment
- **Hardware:** ESP32-S31-Korvo-1 V1.1, or ESP-Mosaico with its camera module
- **SoC module:** ESP32-S31 (dual-core RISC-V, 16 MB flash, 16 MB PSRAM)
- **SDK:** ESP-IDF 6.2 development line (preview support for ESP32-S31)
- **Target:** `esp32s31`
- **Toolchain:** Docker-based DevContainer (`.devcontainer/devcontainer.json`, image `espressif/idf:latest`)
- **Build host:** Linux x86_64 (inside DevContainer)

### 1.3 Glossary
| Term | Definition |
|------|------------|
| WakeNet | ESP-SR voice activation / keyword spotting component |
| ESP-SR | Espressif's Speech Recognition framework |
| MultiNet | ESP-SR speech command recognition model |
| AFE | ESP-SR Audio Front-End (AEC, VAD, noise suppression) |
| ESP-DL | Espressif's deep-learning inference framework |
| YOLO11n | Object detection model shipped by the demo — 320×320 int8, via `espressif/coco_detect` on ESP-DL |
| BSP | Board Support Package — handles all low-level hardware init |
| Korvo-1 | ESP32-S31 devkit with camera, LCD, mic, speaker, and audio codec |
| ESP-Mosaico | ESP32-S31 devkit with a 480×480 LCD and a hot-pluggable camera module |
| DevContainer | Docker-based development environment (VSCode Dev Containers) |
| ES8389 | Audio codec on Korvo-1, connected via I2S + I2C |
| ES8311 | Audio codec on ESP-Mosaico, connected via I2S + I2C |
| DVP | Digital Video Port — parallel camera interface (Korvo-1) |
| PPA | Pixel Processing Accelerator — the ESP32-S31 block that scales and converts frames |

---

## 2. Functional Requirements

### REQ-001: ESP-IDF Project Scaffold with DevContainer

Create the ESP-IDF project skeleton using `idf.py create-project` inside the DevContainer, targeting `esp32s31`.

**Acceptance Criteria:**
- [x] Project scaffolded with `idf.py create-project` inside the DevContainer (IDF 6.2) — the generated `CMakeLists.txt` / `main/` layout is what the repository still uses
- [x] Chip target set to `esp32s31` — carried by each preset in `CMakePresets.json` (`IDF_TARGET`), so there is no separate `set-target` step
- [x] `.devcontainer/devcontainer.json` (single file — no `Dockerfile`, no `docker-compose.yml`) configured for `espressif/idf:latest`
- [x] Standard ESP-IDF `.gitignore` — covering `build/`, `sdkconfig`, `__pycache__/`, `.pytest_cache/`, `dependencies.lock`, `pytest_embedded_log/`, etc.
- [x] `main/` prints a boot banner with chip info, SDK version, and free heap (SRAM and PSRAM) — `App::print_banner()` in `main/app.cpp`
- [x] Project compiles successfully: `idf.py --preset korvo1 build` and `idf.py --preset mosaico build`

---

### REQ-002: BSP Integration and Splash Screen

Add the **ESP32-S31-Korvo-1 BSP** as an IDF component dependency and initialise the board. After boot, display a splash screen for 5 seconds with a demo description message.

The BSP handles all underlying hardware init (audio codec, LCD, touch, camera, SD card, buttons, LED) — no low-level HAL development is needed in this project. The ESP-Mosaico BSP is reached the same way, through its own devkit manifest.

**Acceptance Criteria:**
- [x] BSP component added as a component-registry dependency — declared in the selected devkit's manifest (`devkits/korvo1/idf_component.yml` → `espressif/esp32_s31_korvo_1` 1.0.1; `devkits/mosaico/idf_component.yml` → `esp-mosaico-bsp`, pinned commit), rather than in `main/`
- [x] Individual BSP init functions called per peripheral (`bsp_i2c_init()`, `bsp_audio_init()`, `bsp_display_start()`, camera via the BSP's `esp_video`, etc.) — each returns `esp_err_t`, serving as a hardware self-test
- [ ] Any init failure is logged with `ESP_LOGE` **and `app_main` returns** — the shipped path uses `ESP_ERROR_CHECK()` (`main/app.cpp`), so a fatal peripheral init logs and then aborts rather than returning
- [x] All peripherals initialised at startup — including the camera (Korvo-1: OV3660 over DVP via `esp_video`; ESP-Mosaico: the left-slot camera module), audio, display, touch
- [x] LVGL and touch initialised via the BSP — GT1151 on the Korvo-1, CST9220 on the ESP-Mosaico
- [x] Splash screen displayed for 5 s (or until tap) showing "TinyML Demo / Voice Triggered / Object Detection" (`ui::text::kSplashTitle`), using the full panel
- [x] Tap anywhere on screen skips the splash immediately and WakeNet listening begins

---

### REQ-003: Application State Display

The application shall display the current operating state on the LCD at all times, so the user always knows what the system is doing. State text shall update instantly with no blank screen gaps between transitions. All display output goes through LVGL (initialised by the BSP in REQ-002).

**Acceptance Criteria:**
- [x] The following states are displayed on screen as the application progresses — the wording is byte-for-byte the strings in `main/ui_theme.hpp`:
  - "Listening for wake word..."
  - "Listening for command..."
  - "Taking photo..."
  - "Analysing image..."
  - "No Detection" in the detection list (when inference completes with no results — the status line returns to "Listening for command...")
    — the SRS's own "No objects detected" wording is not used: the list carries the no-result case instead
  - Detection results with bounding box overlay (when objects are found)
- [x] State transitions are instant — no blank screen between states; the status and directive lines are written from the LVGL task (or marshalled to it with `lv_async_call`)
- [x] Splash screen (REQ-002) transitions directly into the first state ("Listening for wake word...") — the main screen is built by the splash's dismissal handler

---

### REQ-004: WakeNet Wake-Word Detection

After the BSP initialisation (REQ-002), integrate **ESP-SR** for always-on wake-word detection using the BSP's audio pipeline. The ESP-SR **Audio Front-End (AFE)** shall be initialised first to handle AEC, noise suppression, and audio buffering; WakeNet then sits on top of AFE.

**Acceptance Criteria:**
- [x] ESP-SR component (`espressif/esp-sr`) added, supporting ESP32-S31 target — pinned 2.4.7 in `main/idf_component.yml`
- [x] ESP-SR AFE initialised at 16 kHz — **mono**, not dual-microphone: the Korvo-1 has a dual-microphone array and the ESP-Mosaico one onboard microphone, but the pipeline runs one channel (`devkits/*/audio_*.cpp` report `afe_mic_format()` = `"M"`)
- [x] WakeNet model loaded and initialised — wake word **"Hi,ESP"** (`CONFIG_SR_WN_WN9_HIESP=y` in `sdkconfig.defaults`), selectable via `menuconfig`
- [x] Continuous audio capture from the onboard microphone via the devkit's audio pipeline — a dedicated feed task on core 0
- [x] On wake word match → callback triggers MultiNet listening phase
- [x] **15-second timeout:** if no command detected within 15 s, fall back to WakeNet listening for wake word (`xTimerCreate("voice_timeout", pdMS_TO_TICKS(15000), …)`)
- [x] Timeout resets / pushes forward during active processing (command detected, capturing, detecting)
- [x] A displayed result does **not** suppress that timeout: with a capture on screen the status line still falls back to wake-word listening when it fires, while the image stays (issue #43)
- [x] Wake word and detection threshold are configurable — `set_wakenet_threshold(afe_data_, ch, 0.3f)` in `main/voice_pipeline.cpp` (code constants, not `menuconfig`)

---

### REQ-005: MultiNet Command Detection ("cheese")

After WakeNet detects the wake word, activate **ESP-SR MultiNet** to listen for the "cheese" command. The audio pipeline (already initialised by the BSP in REQ-002) is reused — no additional hardware setup needed.

**Acceptance Criteria:**
- [x] ESP-SR MultiNet model loaded and initialised — `CONFIG_SR_MN_EN_MULTINET7_QUANT=y`, filtered at runtime with `esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH)`
- [x] MultiNet activated on WakeNet wake-word match callback — created inside the detect task, entered on `WAKENET_CHANNEL_VERIFIED`
- [x] Command **"cheese"** registered in MultiNet's command set — alongside the model's own 32, so 33 are registered in total and only "cheese" triggers a capture
- [x] On "cheese" detected → callback triggers camera capture
- [x] On no command within 15 s → fall back to WakeNet (per REQ-004 timeout)
- [x] MultiNet confidence threshold is configurable — `multinet->set_det_threshold(model_data, 0.3f)` (code constant, not `menuconfig`)

---

### REQ-006: Camera Capture

On the "cheese" command detected by MultiNet, capture a still image from the onboard camera. The camera was already initialised at startup by the BSP in REQ-002 — no additional DVP or sensor configuration needed at capture time.

**Acceptance Criteria:**
- [x] On "cheese" callback → capture a single still frame from the already-initialised camera, through the devkit's `devkit::Camera::capture()`
- [x] Frame buffer allocated in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`)
- [x] Captured frame available in RGB for both display and detection pipeline — two conversions, `to_model_input()` and `to_display()`
- [x] Resolution: **the frame the devkit's camera path produces** — 240×240 on the Korvo-1 (OV3660), 320×180 on the ESP-Mosaico (its PPA scaler only lands on whole 1/16 steps). Not QVGA; the geometry is reported by `devkit::Camera::output_geometry()` and everything downstream derives from it
- [x] Capture completes without blocking the UI — the cycle runs on a dedicated capture worker task, and the dequeue wait is bounded in the driver (`VIDIOC_S_DQBUF_TIMEOUT`). Capture itself is not separately timed in the log; the `CVT:` and `INF:` lines time the conversions and the inference

---

### REQ-007: YOLO11n Object Detection via ESP-DL

Run object detection on the captured camera frame using **YOLO11n** (320×320, int8) through the **coco_detect** component on **ESP-DL** (v3.3.8+, supports `esp32s31` target). The image is already in PSRAM from REQ-006 — pass it to the inference pipeline.

**Acceptance Criteria:**
- [x] ESP-DL component added — pinned `espressif/esp-dl` 3.3.8 in `main/idf_component.yml`
- [x] YOLO11n model loaded (pre-quantized, via `espressif/coco_detect` 0.4.0) — `COCODetect(YOLO11N_320_S8_V1, false)`, read from the `coco_det` flash partition (coco_detect's `model location` Kconfig set to the partition), not linked into the app image
- [x] Model input tensor populated from the camera frame buffer
- [x] Inference runs on-device (no cloud) — in a dedicated PSRAM-backed FreeRTOS task
- [x] Detection results: bounding boxes, class labels, confidence scores
- [ ] Top-N detections (N configurable, default 2) kept for display — **not implemented**: every detection that fits the list column is shown, and the count is whatever the model returned
- [x] Inference completes within acceptable latency (< 5 s) — ≈2.3 s on the Korvo-1 at 240×240 and ≈1.9 s on the ESP-Mosaico at 320×180, from the `INF:` lines the detector logs per capture

---

### REQ-008: LCD Display — Image + Detection Results

Display the captured camera frame on the LCD, overlaid with object detection bounding boxes, labels, and confidence scores (from REQ-007). The BSP already initialised display, LVGL, and touch in REQ-002.

**Acceptance Criteria:**
- [x] Captured camera frame rendered as background image on the LCD
- [x] Detection bounding boxes drawn as coloured rectangles overlaid on the image
- [x] Each detection shown as its **object name and confidence score** in the list beside the image, colour-matched to its bounding box by a swatch. The name is the model's own class list (80 COCO classes); a class the list does not know shows its index and is logged, never an invented name
- [x] No-detection case: "No Detection" in the detection list
- [x] **The result persists — there is no auto-dismiss.** The frame, its boxes and its list stay on screen with "tap to clear" under the picture until the user clears them
- [x] Tap anywhere clears the result immediately and returns to WakeNet listening. Detecting "cheese" also clears it, at the moment the command is detected, so the countdown runs over the white placeholder

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
- ESP-DL includes the YOLO11n `.espdl` model that `espressif/coco_detect` ships; nothing else

**Model storage (flash):**
- A `model` partition is defined in each devkit's `partitions.csv` with 4 MB for the ESP-SR models,
  and a `coco_det` partition of 3 MB for the YOLO11n detection model
- Both are flashed to their partitions and read at runtime — they are not part of the application
  binary. The ESP-SR models are mounted as SPIFFS and read by `esp_srmodel_init`; the YOLO11n model is
  memory-mapped from `coco_det` by ESP-DL's `fbs_loader`

**Runtime memory allocation:**
- **SRAM (internal):** reserved for time-critical audio paths:
  - WakeNet inference buffers
  - Audio pipeline ring buffers
  - RTOS kernel, stacks
- **PSRAM (16 MB):** for large, less latency-sensitive data:
  - Camera frame buffer(s) and the display/detector conversions
  - YOLO/ESP-DL tensor arena and weight buffers
  - LVGL framebuffer / draw buffers
  - The voice pipeline's two task stacks
- ESP-DL's static memory planner is configured with an internal SRAM budget; the rest goes to PSRAM

### 3.2 Memory Monitoring

After each major software component initialises (BSP, WakeNet, MultiNet, camera, ESP-DL/YOLO), a single-line memory summary shall be printed to UART showing:
- Free SRAM (internal)
- Free PSRAM
- Largest free heap block
- **Warning** if either SRAM or PSRAM usage exceeds 80% of available capacity

*Shipped as `App::print_memory_summary()`, called after each bring-up stage (`I2C`, `AUDIO`, `DISPLAY+LVGL`, `CAMERA`, `DETECTOR`, `VOICE`).*

### 3.3 Performance Logging

Where applicable, timing shall be printed to UART for observability:
- **Camera conversions:** one `CVT:` line per capture, with the produced geometry
- **YOLO/ESP-DL:** one `INF:` line per inference, with the detection count
- Pre-process, inference and post-process are **not** reported individually — inference is one call into `COCODetect::run()`
- **Audio path diagnostics:** behind `CONFIG_AUDIO_METRICS` (off by default), the pipeline logs per-channel peak/mean/clipped levels

### 3.4 Error Handling

- All errors shall be logged with `ESP_LOGE` (not `ESP_LOGW` or `ESP_LOGI`)
- **Init phase errors** (BSP init, WakeNet model load, MultiNet model load, camera init, ESP-DL init): log and stop — the shipped path uses `ESP_ERROR_CHECK()` for peripheral init, which aborts
- **Operation phase errors** (inference failure, capture failure): skip the current operation cycle and return to WakeNet wake-word listening — do not halt the system

### 3.5 Development Language & Architecture

- **Language:** C++17
- **Architecture:** Modern object-oriented principles with clear separation of concerns
- Each logical part is encapsulated in a class: `App`, `VoicePipeline`, `Detector`, `MainScreen`, `Splash`, and the devkit interfaces `Board` / `Audio` / `Display` / `Camera`
- **Hardware configurability:** the devkit is selected by the build preset (`CMakePresets.json`) — `idf.py --preset korvo1|mosaico`. The application code contains no board-specific logic: it talks to `components/devkit_api`, and each devkit provides the implementation
- Automated unit / integration tests are **deferred** — tracked as a release-checklist item in issue #49

---

## 4. Edge Cases

| # | Scenario | Current behaviour | Potential issue | Proposed handling |
|---|----------|-------------------|----------------|-------------------|
| EC-001 | "cheese" detected while already processing a previous capture/detect/display cycle | **Guarded** — the capture worker sets `cb_busy_` and refuses a second trigger while one is in flight; `Detector` refuses a frame while an inference is running. The screen reports "Camera not responding" instead of queueing | A refused cycle looks like a camera fault to the user | Keep the guard; report the refused cycle distinctly from a camera error |
| EC-002 | User taps screen during "Analysing image" or "Taking photo" state | Tap should dismiss or advance | Interrupts processing mid-way, unpredictable state | Ignore — no action taken during active processing states |
| EC-003 | User says wake word ("Hi ESP") while already in an active cycle (not in "Listening for wake word" state) | Audio fed to WakeNet — could trigger a second detection | Double wake word confuses state machine, could launch parallel cycles | Ignore during active cycle — audio data shall not be fed to any model when in an invalid state |
| EC-004 | Camera fails to capture frame (sensor timeout, frame not ready) | The dequeue wait is bounded in the driver; a frameless capture fails and is logged, and the UI returns to the wake-word state | App stuck waiting for frame | Skip cycle: log error, return to WakeNet listening (per NFR 3.4) |
| EC-005 | YOLO inference exceeds acceptable latency | No timeout around the inference call itself | Inference hangs, app freezes | Use a timeout (max 5 s). If exceeded, abort inference, log error, and return to WakeNet listening. |
| EC-006 | PSRAM allocation failure at startup (camera buffer, YOLO tensor arena) | App exits with ESP_LOGE | App won't run on boards with insufficient PSRAM | **Deal-breaker** — must be solved before release. Not acceptable as runtime behaviour. |
| EC-007 | Touch controller becomes unresponsive at runtime (not init failure) | Tap gesture never fires — the user cannot manually clear a result | App relies solely on timeouts to progress states | Accept degraded behaviour — the splash auto-advances at 5 s; a **result does not auto-advance**, it stays until the next "cheese" (issue #43) |

## 5. Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | TBD | Ozan | Initial draft |
| 1.1 | 2026-10-17 | Ozan | Claims corrected against the tree; acceptance criteria marked against the implementation; second devkit (ESP-Mosaico) reflected (issue #50) |

---

## 6. References

See the per-devkit reference list in the [README](../README.md#references) — the SRS no longer keeps
its own copy, so the two cannot drift apart.
