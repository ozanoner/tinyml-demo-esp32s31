# TinyML Demo — ESP32-S31

A voice-triggered object detection demo for the ESP32-S31 family: the
**ESP32-S31-Korvo-1** (800×480 LCD) and the **ESP-Mosaico** (480×480 LCD). Say
the wake word, then speak "cheese" to capture a camera frame, run YOLO11n
inference on it, and show the frame with its detections on the LCD.

## Scope and Features

- **Wake word detection** — WakeNet9 ("Hi, ESP") triggers command mode
- **Command detection** — MultiNet7 classifies the phrase (10 English commands)
- **Camera capture** — Only the "cheese" command triggers the camera
- **Object detection** — YOLO11n 320×320 via ESP-DL (`espressif/coco_detect`),
  ~3.2 s inference, COCO 80-class model
- **LCD display** — one main screen in two rows: the picture (with its coloured
- **LCD display** — one main screen in two rows: the picture (with its coloured
  bounding boxes and its own "tap to clear" line) beside the detection list, over
  a status line and a state directive line. One layout for both devkits, derived
  from the panel's own geometry
- **Named detections** — each row shows the model's own class name (`person`,
  `cat`, `traffic light`, …) and its accuracy
- **Persistent result** — the capture stays on screen until a tap clears it or the
  next "cheese" replaces it. There is no auto-dismiss

## Tech Stack

| Component | Detail |
|-----------|--------|
| SoC | ESP32-S31 (RISC-V dual-core, LP core, 300 MHz) |
| Devkit (korvo1 preset) | ESP32-S31-Korvo-1 V1.1 (16 MB Octal PSRAM, 16 MB flash, 800×480 LCD + touch, OV3660 camera, ES8389 dual-mic codec) |
| Devkit (mosaico preset) | ESP-Mosaico (16 MB Octal PSRAM, 16 MB flash, 480×480 LCD + touch, camera module, ES8311 codec) |
| SDK | ESP-IDF 6.2 development line (`espressif/idf:latest`, `idf.py --version` → `v6.1-dev-8440-gab95e70748f`), target `esp32s31` |
| GUI | LVGL 9.5.0 (`lvgl/lvgl`, pinned in `main/idf_component.yml`) |
| BSP | `espressif/esp32_s31_korvo_1` 1.0.1; `esp-mosaico-bsp` (pinned commit) |
| Voice | ESP-SR: WakeNet9_HIESP (wake word), MultiNet7_Quant (commands), VAD, AEC, NS |
| ML | ESP-DL (`espressif/esp-dl` 3.3.8) + `espressif/coco_detect` 0.4.0 — YOLO11n 320×320 int8 |
| Touch | GT1151 (Korvo-1), CST9217 (ESP-Mosaico) |
| Codec | ES8389 (Korvo-1), ES8311 (ESP-Mosaico) |

## Solution Architecture

The application initialises all BSP peripherals in `app_main`, shows a splash
screen, then hands control to a persistent LVGL task and independent
subsystems:

```
┌─────────────────────────────────────────────────────┐
│                     app_main()                        │
│   init BSP → Splash → MainScreen → Camera →          │
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
│                      MainScreen                        │
│  image row:  picture (produced frame, 1:1) | list      │
│  message row: status line + directive line             │
└───────────────────────────────────────────────────────┘
```

**Key classes:**

- `MainScreen` (`main/main_screen.{hpp,cpp}`) — the main screen: picture,
  its directive line, the detection list, the status line and the state directive
  line. Owns the "cheese" countdown and the capture worker task; the result
  persists until a tap or the next "cheese"
- `object_labels.hpp` — the model's own 80 class names, generated from the model's
  metadata by `tools/gen_object_labels.py`, never hand-transcribed
- `ui_theme.hpp` / `ui_layout.hpp` — the visual system in one place (palette, type
  scale, screen texts) and the placement derived from hardware facts: the panel
  geometry the devkit reports and the frame geometry the camera produces
- `VoicePipeline` — owns AFE, WakeNet9, MultiNet7; runs feed + detect tasks
- `Camera` (`devkit::Camera`) — per-devkit capture + the two conversions
  (detector image, display image); reports the geometry it produces
- `Detector` — runs YOLO11n inference in a PSRAM-backed FreeRTOS task
- `Splash` — boot-time splash screen, dismissed by tap or 5 s timeout

**Where the screen's numbers come from.** The application holds no panel
coordinates. Each devkit reports its panel size (`devkit::Display::width()` /
`height()`) and the frame geometry its capture path produces
(`devkit::Camera::output_geometry()`: the sensor's own frame on Korvo-1, a quarter
of it on the ESP-Mosaico, where the PPA scaler only lands on whole 1/16 steps).
`ui::compute_layout()` turns those two facts into the picture box, the detection
column, the divider and the message row.

**Fonts** (`sdkconfig.defaults`, named in `ui_theme.hpp`): Montserrat 28 for the
status line, 20 for the directive line, 14 (LVGL's default) for the detection
list — 28 and 14 were already enabled, 20 is added.

## ML Models

| Model | Type | Source | Purpose |
|-------|------|--------|---------|
| WakeNet9 | CNN | ESP-SR (`WN9_HIESP`) | Wake word "Hi, ESP" detection |
| MultiNet7 | CNN | ESP-SR (`MN_EN_MULTINET7_QUANT`) | 10 English command classification, int8 quantised |
| YOLO11n 320×320 | CNN | `espressif/coco_detect` 0.4.0 | 80-class COCO object detection, int8 quantised |

All models run on the RISC-V CPU (no hardware accelerator). The YOLO11n model
is embedded in the firmware at build time (~7.2 MB in the `coco_detect`
partition).

The detection list shows what the detector reports — a category index and a
score, e.g. `obj 42  87%`. The COCO class names are not mapped to those indices
in this project yet.

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

Each build excludes the other devkit (`EXCLUDE_COMPONENTS` in the top-level
`CMakeLists.txt`), because both BSPs export the same symbols (`bsp_display_new`,
`bsp_display_lock`, `bsp_touch_new`, …) and a build carrying both fails to link.
Each devkit therefore resolves its own version set — Korvo-1 gets
`esp_video 2.2.0` + `esp_codec_dev 1.5.x`, the ESP-Mosaico `esp_video 2.5.x` +
`esp_codec_dev 1.6.x`, the sets issues #36/#38 and #35 were verified against.

Excluding a component also removes its manifest from the dependency solve, so
switching devkits re-solves the graph and prunes the other devkit's components
from `managed_components/` (about 19 of them, re-fetched on that devkit's next
build — measured at ~80 s). Each devkit's build directory, object files and image
survive the switch, and there is no `set-target` and no full clean.

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
3. Say the wake word **"Hi, ESP"** — the status line shows "Listening for command..."
4. Say **"cheese"** — a 3-second countdown runs in the status line
5. "Analysing image..." while YOLO11n runs (~3.2 s); the picture box stays white
   until there is a frame to show
6. The main screen shows the captured frame with its bounding boxes, the
   detections listed beside it, and "tap to close" below
7. Tap anywhere or wait 10 s — the result is dismissed and the picture box goes
   back to white

### Capture serial logs

```bash
# On the host connected to the devkit:
cd ~/logs
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 \
  | tee tinyml-demo_$(date +%Y%m%d_%H%M%S).log
```

Press Ctrl-] to exit miniterm.

On a devkit whose console is not wired to a USB port — the ESP-Mosaico's camera
module claims the USB Serial/JTAG pads (`devkits/mosaico/sdkconfig.defaults`) —
the console is UART only and has to be read through a UART bridge.

## References

- [ESP32-S31 Korvo-1 BSP](https://components.espressif.com/components/espressif/esp32_s31_korvo_1)
- [ESP-IDF Programming Guide (6.2 development line)](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP-SR Speech Recognition](https://github.com/espressif/esp-sr)
- [ESP-DL Deep Learning Library](https://github.com/espressif/esp-dl)
- [COCODetect Component](https://components.espressif.com/components/espressif/coco_detect)
- [LVGL Documentation](https://docs.lvgl.io/9.x/)
- [ESP32-S31 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s31_datasheet_en.pdf)
