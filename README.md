# TinyML Demo — ESP32-S31 Korvo-1

A voice-triggered object detection demo for the ESP32-S31 Korvo-1 devkit. Speak a command ("cheese", "turn on", etc.) to capture a camera frame, run COCO YOLO11n inference, and display bounding boxes on the 800×480 LCD.

## Scope and Features

- **Voice activation** — WakeNet detects the wake word, transitions to command mode
- **Command recognition** — MultiNet detects commands ("cheese", "turn on", "turn off", etc.)
- **Camera capture** — OV3660 sensor, 240×240 RGB565 frames
- **Object detection** — COCO YOLO11n via ESP-DL (`espressif/coco_detect`), ~3.2 s inference
- **LCD display** — 800×480 RGB LCD with LVGL: camera image centred, coloured bounding boxes, class labels next to the image
- **Auto-dismiss** — 10 s timer or tap anywhere returns to wake-word listening

## Tech Stack

| Component | Detail |
|-----------|--------|
| SoC | ESP32-S31 (RISC-V dual-core, 300 MHz) |
| Devkit | ESP32-S31-Korvo-1 (16 MB PSRAM, 16 MB flash, LCD, touch, camera, dual-mic) |
| SDK | ESP-IDF v6.1 (target `esp32s31`) |
| GUI | LVGL 9.x (`lvgl/lvgl`) via `espressif/esp_lvgl_port` |
| BSP | `espressif/esp32_s31_korvo_1` |
| Voice | ESP-SR: WakeNet (wake word), MultiNet (commands), VAD, AEC, NS |
| ML | ESP-DL (`espressif/esp-dl`) + COCODetect (`espressif/coco_detect`) — YOLO11n |
| Camera | OV3660 via `espressif/esp_cam_sensor` |
| Touch | GT1151 via `espressif/esp_lcd_touch_gt1151` |
| Display | ST7262 via SPI + DMA |

## Solution Architecture

The application is structured around a persistent LVGL task with independent subsystems:

```
┌─────────────────────────────────────────────────────┐
│                     app_main()                        │
│   init BSP → Splash → StateDisplay → Camera →        │
│   Detector → VoicePipeline → return                   │
└─────────────────────┬─────────────────────────────────┘
                      │
    ┌─────────────────┼──────────────────┐
    ▼                 ▼                   ▼
┌─────────┐   ┌──────────────┐   ┌──────────────┐
│ Voice   │   │   Camera+    │   │  LVGL Port   │
│Pipeline │   │  Detector    │   │    Task      │
│ (Core 1)│   │ (Core 0)     │   │ (any core)   │
└────┬────┘   └──────┬───────┘   └──────┬───────┘
     │                │                  │
     │ WakeNet/       │ infer callback   │ lv_async_call /
     │ MultiNet       │ via lv_async_call│ lv_timer
     ▼                ▼                  ▼
┌───────────────────────────────────────────────────────┐
│                    StateDisplay                        │
│     Persistent LVGL label for app state messages       │
└───────────────────────────────────────────────────────┘
```

**Key classes:**

- `StateDisplay` — persistent text label at screen centre (theme-inherited font)
- `VoicePipeline` — owns AFE, WakeNet, MultiNet; runs feed + detect tasks
- `Camera` — wraps OV3660 sensor, pre-allocates PSRAM frame buffer
- `Detector` — runs COCO YOLO11n inference in its own PSRAM-backed FreeRTOS task
- `ResultDisplay` — transient LVGL overlay: camera frame, boxes, labels, 10 s auto-dismiss
- `Splash` — boot-time splash screen, dismissed by tap or 5 s timeout

## ML Models

| Model | Type | Source | Purpose |
|-------|------|--------|---------|
| WakeNet | CNN | ESP-SR (`wakenet8`) | Wake word detection |
| MultiNet | CNN | ESP-SR (`multinet6`) | Command classification (10 EN commands) |
| YOLO11n (COCO) | CNN | `espressif/coco_detect` 0.4.0 | 80-class object detection, ~3.2 s/frame |

All models run on the RISC-V CPU (no hardware accelerator). The YOLO11n model is embedded in the firmware binary (~7.2 MB).

## Build & Test

### Prerequisites

- ESP-IDF v6.1 with `esp32s31` target support
- Docker (optional — build inside container `tinyml-demo-esp32s31`)
- `esptool` 5.3+ on the target machine
- Python 3 for serial capture

### Build

```bash
cd tinyml-demo-esp32s31
idf.py set-target esp32s31
idf.py build
```

### Flash (app only)

```bash
# Copy binary to the test machine
scp build/tinyml-demo-esp32s31.bin cirunner:~/firmware/

# Flash (921600 baud, app partition at 0x10000)
ssh cirunner "esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 ~/firmware/tinyml-demo-esp32s31.bin"
```

### Run the demo

1. Power on the devkit — the splash screen appears
2. Tap the screen or wait 5 s for the splash to dismiss
3. Say the wake word (default: "Alexa") — the display shows "Listening for command..."
4. Say a command (e.g. "cheese") — the 3-second countdown begins, then the camera captures
5. The display shows "Analysing image..." while YOLO runs (~3.2 s)
6. Result screen: camera frame centred, bounding boxes overlaid, labels on the right
7. Tap anywhere or wait 10 s to return to command mode

### Capture serial logs

```bash
ssh cirunner "cd ~/logs && timeout 15 python3 ~/serial_capture.py \
  > tinyml-demo_$(date +%Y%m%d_%H%M%S).log 2>&1"
```

## References

- [ESP32-S31 Korvo-1 BSP](https://components.espressif.com/components/espressif/esp32_s31_korvo_1)
- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP-SR Speech Recognition](https://github.com/espressif/esp-sr)
- [ESP-DL Deep Learning Library](https://github.com/espressif/esp-dl)
- [COCODetect Component](https://components.espressif.com/components/espressif/coco_detect)
- [LVGL Documentation](https://docs.lvgl.io/9.x/)
- [ESP32-S31 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s31_datasheet_en.pdf)
