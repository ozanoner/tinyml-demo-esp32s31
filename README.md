# TinyML Demo — ESP32-S31

Say **"Hi, ESP"**, then **"cheese"**, and the board takes a photo and shows you what
YOLO11n found in it — bounding boxes and labels on the LCD, all on-device, no cloud.

One application, two devkits: the **ESP32-S31-Korvo-1** (800×480) and the **ESP-Mosaico**
(480×480). The build preset picks the board; the application code is identical.

![Architecture: one app, two devkits](docs/architecture/architecture.png)

## What it does

- **Wake word** — WakeNet9 ("Hi, ESP") arms command mode
- **Command** — MultiNet7 listens for one of 33 registered phrases; only **"cheese"** takes a photo
- **Capture** — a still frame from the devkit's camera, converted once for the model and once for the screen
- **Detection** — YOLO11n 320×320 int8 through ESP-DL / `coco_detect`, ≈2.3 s on the Korvo-1 and ≈1.9 s on the ESP-Mosaico
- **Result** — the picture with its boxes, a named detection list beside it, and "tap to clear". The
  result stays until you tap or say "cheese" again — there is no auto-dismiss

## Run it in 60 seconds

Requires Docker and a devkit over USB. Nothing is installed on the host — the toolchain lives in
the container, and it needs the USB device passed through.

```bash
# 1. Build + flash, inside the container (korvo1, or --preset mosaico)
docker run --rm -it -v "$PWD":/project -w /project --device=/dev/ttyUSB0 \
  espressif/idf:latest bash
idf.py --preset korvo1 -p /dev/ttyUSB0 flash

# 2. Watch it boot — Ctrl-] to exit
idf.py --preset korvo1 -p /dev/ttyUSB0 monitor
```

`idf.py flash` writes the whole set the build produced (bootloader, partition table, ota_data, the
ESP-SR models and the app) at the offsets in `build/<devkit>/flash_args`, and skips the partitions
that are already up to date. After a change to application code alone, `idf.py --preset <devkit>
-p /dev/ttyUSB0 app-flash` writes only the app partition.

`idf.py monitor` reads the console at the right baud rate and hands the ELF to the panic decoder, so
a crash prints a backtrace with function names instead of raw addresses.

A VS Code DevContainer does **not** forward USB devices by default — it needs `--device=/dev/ttyUSB0`
in its `runArgs` before flash or monitor can reach a devkit from inside it.

Say "Hi, ESP", then "cheese". The status line counts down, the picture box fills, and the
detections appear beside it.

## Supported devkits

| | ESP32-S31-Korvo-1 | ESP-Mosaico |
|---|---|---|
| Preset | `korvo1` | `mosaico` |
| Panel | 800×480 RGB LCD, GT1151 touch | 480×480 CO5300 QSPI panel, CST9217 touch |
| Camera | OV3660 over DVP, 240×240 RGB565_BE | left-slot camera module (OV3640 / SC101IOT), 320×180 |
| Audio codec | ES8389 | ES8311 |
| BSP | `espressif/esp32_s31_korvo_1` 1.0.1 | `esp-mosaico-bsp` @ `c32332e` |

Both: ESP32-S31, 16 MB flash, 16 MB octal PSRAM, target `esp32s31`. Both have two microphones;
the pipeline runs one channel.

## How it is built

`main/` holds the application: `App` owns and wires everything, `VoicePipeline` runs the two audio
tasks, `Detector` owns the inference task, `MainScreen` and `Splash` are the LVGL surfaces.
`components/devkit_api` is the board seam — `Board` / `Audio` / `Display` / `Camera` — and
`devkits/korvo1` and `devkits/mosaico` implement it.

Both diagrams are generated from this repository and source-backed: every component carries
the file and line range that proves it. They are **generated files** — edit the JSON, never the HTML.

| Diagram | Static render | Interactive | What it answers |
|---|---|---|---|
| `architecture` | [architecture.png](docs/architecture/architecture.png) | [architecture.html](docs/architecture/architecture.html) | What the system is made of: the application layer, `components/devkit_api` as the only board seam, the two devkit implementations, `esp_video`, and the third-party stack |
| `runtime` | [runtime.png](docs/architecture/runtime.png) | [runtime.html](docs/architecture/runtime.html) | Where data crosses tasks: feed → AFE → detect → command → countdown → capture → conversions → inference → screen |

The runtime path, task by task:

![Runtime: voice to photo across tasks](docs/architecture/runtime.png)

GitHub does not render an `.html` blob, so the interactive files open through htmlpreview — replace
`github.com` with `htmlpreview.github.io/?` in the blob URL:

- [architecture — interactive](https://htmlpreview.github.io/?https://github.com/ozanoner/tinyml-demo-esp32s31/blob/main/docs/architecture/architecture.html)
- [runtime — interactive](https://htmlpreview.github.io/?https://github.com/ozanoner/tinyml-demo-esp32s31/blob/main/docs/architecture/runtime.html)

Downloading one and opening it locally works too, and needs no network: each is a single
self-contained HTML page.

## Documents

- [docs/building.md](docs/building.md) — how one app builds for two devkits, flashing, serial logs, regenerating the diagrams
- [docs/requirements.md](docs/requirements.md) — the SRS, with every acceptance criterion marked against the code

## References

**Shared**

- [ESP-IDF Programming Guide (6.2 development line)](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [ESP-SR — WakeNet, MultiNet, AFE](https://github.com/espressif/esp-sr)
- [ESP-DL — on-device inference](https://github.com/espressif/esp-dl)
- [COCODetect component (`espressif/coco_detect`)](https://components.espressif.com/components/espressif/coco_detect)
- [esp_cam_sensor — the camera sensor layer both devkits use](https://components.espressif.com/components/espressif/esp_cam_sensor)
- [LVGL 9.5 documentation](https://docs.lvgl.io/9.5/)
- [ESP32-S31 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s31_datasheet_en.pdf)

**ESP32-S31-Korvo-1**

- [Korvo-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/user_guide.html)
- [Korvo-1 BSP — `espressif/esp32_s31_korvo_1`](https://components.espressif.com/components/espressif/esp32_s31_korvo_1)
- [Korvo-1 BSP source and examples (`esp-bsp`)](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s31_korvo_1)
- [OV3660 sensor](https://www.ovt.com/products/ov3660) — DVP, RGB565_BE 240×240

**ESP-Mosaico**

- [ESP-Mosaico user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html)
- [mosaico.espressif.com](https://mosaico.espressif.com/)
- [ESP-Mosaico BSP source](https://github.com/esp-mosaico/esp-mosaico-bsp) — `components/esp-mosaico-bsp` and `components/mosaico_module_camera`, both at the pinned commit
- Camera module: the left slot's module carries an OV3640 or SC101IOT sensor — see the user guide's camera section

## Licence

MIT — see [LICENSE](LICENSE).
