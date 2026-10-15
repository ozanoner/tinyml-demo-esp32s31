# TinyML Demo — ESP32-S31 Korvo-1

A voice-triggered object detection demo for the ESP32-S31 Korvo-1 devkit. Say
the wake word, then speak "cheese" to capture a camera frame, run YOLO11n
inference, and display bounding boxes on the 800×480 LCD.

## Scope and Features

- **Wake word detection** — WakeNet9 ("Hi, ESP") triggers command mode
- **Command detection** — MultiNet7 classifies the phrase (10 English commands)
- **Camera capture** — Only the "cheese" command triggers the OV3660 sensor
- **Object detection** — YOLO11n 320×320 via ESP-DL (`espressif/coco_detect`),
  ~3.2 s inference, COCO 80-class labels
- **LCD display** — 800×480 RGB LCD with LVGL: camera image centred, coloured
  bounding boxes overlaid, class labels next to the image
- **Auto-dismiss** — 10 s timer or tap anywhere returns to wake word listening

## Tech Stack

| Component | Detail |
|-----------|--------|
| SoC | ESP32-S31 (RISC-V dual-core, LP core, 300 MHz) |
| Devkit | ESP32-S31-Korvo-1 V1.1 (16 MB Octal PSRAM, 16 MB flash, 800×480 LCD + touch, OV3660 camera, ES8389 dual-mic codec) |
| SDK | ESP-IDF 6.2 development line (`espressif/idf:latest`, `idf.py --version` → `v6.1-dev-8440-gab95e70748f`), target `esp32s31` |
| GUI | LVGL 9.6.0~1 (`lvgl/lvgl`) via `espressif/esp_lvgl_port` |
| BSP | `espressif/esp32_s31_korvo_1` 1.0.1 |
| Voice | ESP-SR: WakeNet9_HIESP (wake word), MultiNet7_Quant (commands), VAD, AEC, NS |
| ML | ESP-DL (`espressif/esp-dl` 3.3.8) + `espressif/coco_detect` 0.4.0 — YOLO11n 320×320 int8 |
| Camera | OV3660 via `espressif/esp_cam_sensor` |
| Touch | GT1151 via `espressif/esp_lcd_touch_gt1151` |
| Display | ST7262 LCD via SPI + DMA, 16-bit RGB565 |
| Codec | ES8389 via `espressif/esp_codec_dev` |

## Solution Architecture

The application initialises all BSP peripherals in `app_main`, shows a splash
screen, then hands control to a persistent LVGL task and independent
subsystems:

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

- `StateDisplay` — persistent LVGL label at screen centre (inherits theme font)
- `VoicePipeline` — owns AFE, WakeNet9, MultiNet7; runs feed + detect tasks
- `Camera` — wraps OV3660 sensor, pre-allocates PSRAM frame buffer
- `Detector` — runs YOLO11n inference in a PSRAM-backed FreeRTOS task
- `ResultDisplay` — transient LVGL overlay: camera frame, boxes, labels, 10 s auto-dismiss
- `Splash` — boot-time splash screen, dismissed by tap or 5 s timeout

## ML Models

| Model | Type | Source | Purpose |
|-------|------|--------|---------|
| WakeNet9 | CNN | ESP-SR (`WN9_HIESP`) | Wake word "Hi, ESP" detection |
| MultiNet7 | CNN | ESP-SR (`MN_EN_MULTINET7_QUANT`) | 10 English command classification, int8 quantised |
| YOLO11n 320×320 | CNN | `espressif/coco_detect` 0.4.0 | 80-class COCO object detection, int8 quantised |

All models run on the RISC-V CPU (no hardware accelerator). The YOLO11n model
is embedded in the firmware at build time (~7.2 MB in the `coco_detect`
partition).

## Build & Test

### Prerequisites

- ESP-IDF 6.2 (`espressif/idf:latest`) with `esp32s31` target support
- `esptool` 5.3+ on the host that has the devkit connected by USB

### Build (inside devcontainer)

The project includes a `.devcontainer` with the correct ESP-IDF version.

One CMake preset per devkit selects the devkit, its build directory and its
sdkconfig, so the two devkits build independently — neither build touches the
other's directory:

```bash
cd /project
idf.py --preset korvo1 build     # -> build/korvo1/tinyml-demo-esp32s31.bin
idf.py --preset mosaico build    # -> build/mosaico/tinyml-demo-esp32s31.bin
```

There is no separate `set-target` step and no `DEVKIT` variable to set: each
preset carries the chip target (`esp32s31` in both) and its own
`build/<devkit>/sdkconfig`.

Both devkit directories stay in the build, so one dependency solution covers
both. Two components cannot be satisfied by a single version — `esp_video`
(Korvo BSP `~2.2` vs the ESP-Mosaico camera module `^2.4.1`) and `esp_codec_dev`
(Korvo BSP `~1.5` vs the ESP-Mosaico BSP `~1.6`) — and are resolved by `overrides`
in `main/idf_component.yml`, which names the version and the reason for each.

### Flash (app partition only)

The application image is at offset `0x10000`, in the selected devkit's build
directory.  Flash baud rate up to 921600:

```bash
esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/korvo1/tinyml-demo-esp32s31.bin
```

### Run the demo

1. Power on the devkit — the splash screen appears
2. Tap the screen or wait 5 s for the splash to dismiss
3. Say the wake word **"Hi, ESP"** — the display shows "Listening for command..."
4. Say **"cheese"** — a 3-second countdown begins, then the camera captures
5. The display shows "Analysing image..." while YOLO11n runs (~3.2 s)
6. Result screen: camera frame centred, coloured bounding boxes, labels on the right
7. Tap anywhere or wait 10 s to return to command mode

### Capture serial logs

```bash
# On the host connected to the devkit:
cd ~/logs
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 \
  | tee tinyml-demo_$(date +%Y%m%d_%H%M%S).log
```

Press Ctrl-] to exit miniterm.

## References

- [ESP32-S31 Korvo-1 BSP](https://components.espressif.com/components/espressif/esp32_s31_korvo_1)
- [ESP-IDF Programming Guide (6.2 development line)](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP-SR Speech Recognition](https://github.com/espressif/esp-sr)
- [ESP-DL Deep Learning Library](https://github.com/espressif/esp-dl)
- [COCODetect Component](https://components.espressif.com/components/espressif/coco_detect)
- [LVGL Documentation](https://docs.lvgl.io/9.x/)
- [ESP32-S31 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s31_datasheet_en.pdf)
