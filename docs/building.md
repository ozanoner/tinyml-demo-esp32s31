# Building, flashing and the two-devkit build

The README keeps the short path. This file keeps the detail that only matters when something
goes wrong or when you change how the build works.

## One app, two devkits

`CMakePresets.json` defines one preset per devkit. A preset carries everything the build needs, so
there is no `set-target` step and no `DEVKIT` variable to set by hand:

| Preset | Build directory | `sdkconfig` | `sdkconfig.defaults` |
|---|---|---|---|
| `korvo1` | `build/korvo1` | `build/korvo1/sdkconfig` | `sdkconfig.defaults` + `devkits/korvo1/sdkconfig.defaults` |
| `mosaico` | `build/mosaico` | `build/mosaico/sdkconfig` | `sdkconfig.defaults` + `devkits/mosaico/sdkconfig.defaults` |

Both presets set `IDF_TARGET=esp32s31` and `DEVKIT=<name>`.

The top-level `CMakeLists.txt` fails outright if `DEVKIT` is unset, checks that
`devkits/<DEVKIT>/idf_component.yml` exists, and then:

- puts **only the selected devkit's directory** in `EXTRA_COMPONENT_DIRS`
- names every *other* devkit in `EXCLUDE_COMPONENTS`

`main/CMakeLists.txt` requires `devkit_api` plus `$ENV{DEVKIT}` — unconditionally, from the
environment, never from a `CONFIG_*` value. That is deliberate: IDF evaluates component
CMakeLists twice, and the early-expansion pass runs before `sdkconfig` exists, so a
`CONFIG_`-conditional `REQUIRES` is recorded empty and the devkit never enters the requirement
graph — which shows up as `main/app_main.cpp includes bsp/esp-bsp.h … not in the requirements
list of "main"` and `lvgl.h: No such file or directory`.

**Why the other devkit is excluded rather than coexisting.** Both BSPs export the same symbols —
`bsp_display_new()`, `bsp_display_lock()`, `bsp_touch_new()` — so a build carrying both fails to
link. Excluding one also removes its manifest from the dependency solve, which is what lets each
devkit resolve its own version set (`esp_video` 2.2.0 + `esp_codec_dev` 1.5.x on the Korvo-1,
2.5.x + 1.6.x on the ESP-Mosaico) instead of one compromise version.

**The cost of switching.** Switching devkits re-solves the graph and prunes the other devkit's
components out of `managed_components/`, so they are fetched again on that devkit's next build.
Each devkit's build directory, object files and image survive the switch, and there is no
`set-target` and no full clean.

**There is no `sdkconfig` at the repository root — by design.** Each preset points at its own
`build/<devkit>/sdkconfig`. Nothing is lost by that file's absence.

## Dependency pins

Roots are pinned exactly; each pinned component's own manifest owns its subtree. Exact binary
reproduction of some earlier build is not a goal — these pins are today's resolve, not a promise.

`main/idf_component.yml`:

| Component | Pin | Why here |
|---|---|---|
| `espressif/esp-sr` | 2.4.7 | moves with `esp-dl` (below) |
| `espressif/esp-dl` | 3.3.8 | 3.3.13 strips ops at compile time to whatever registered req files name; only esp-sr registers one, so the `coco_detect` model's Swish op was compiled out, `Model::load()` failed and Detector init panicked on every boot (issue #27) |
| `espressif/coco_detect` | 0.4.0 | 0.4.1 declares `esp-dl ~3.3.13`; 0.4.0 and 0.4.1 ship identical sources and an identical model, only the declared range differs |
| `lvgl/lvgl` | 9.5.0 | `main/` includes `lvgl.h` directly, so the version is named here; it must sit in the intersection of `esp_lvgl_port` (`>=8,<10`) and the ESP-Mosaico BSP (`^9.5.0,<9.6.0`), where 9.5.0 is the only published version |

The BSP itself is owned by the selected devkit: `espressif/esp32_s31_korvo_1` 1.0.1 in
`devkits/korvo1/idf_component.yml`, and `esp-mosaico-bsp` + `mosaico_module_camera` pinned to
commit `c32332e9483edd10f0351ab0265f8b2ddb5992b1` in `devkits/mosaico/idf_component.yml`.

There is deliberately **no `overrides` block** in `main/idf_component.yml`. Two components would
need one if both devkits were solved together (`esp_video` and `esp_codec_dev`), but the excluded
devkit's manifest is never read, so each devkit gets its own set. Forcing one version across both
would hold the ESP-Mosaico camera module — the Mosaico's whole camera path — below the range it
declares.

`dependencies.lock` is gitignored and rewritten by each devkit's build, so it always records the
devkit that was built last.

## The Korvo-1 DVP patch, and why `idf.py --version` says `-dirty`

`devkits/korvo1/CMakeLists.txt` applies `devkits/korvo1/patches/esp-idf-dvp-frame-capture-stability.patch`
to `$ENV{IDF_PATH}` at configure time with `git apply --no-index`. It is idempotent (checked with
`--reverse --check` first) and it **warns and continues** if `git` is missing, the tree is
read-only, or the patch conflicts — in which case the build carries the stock driver.

Consequence: the IDF tree inside the image is modified, so the version string carries `-dirty`:

```
$ idf.py --version
ESP-IDF v6.1-dev-8440-gab95e70748f-dirty
```

and `git status` in the IDF tree shows exactly three files under
`components/esp_driver_cam/dvp/`. The ESP-Mosaico camera module ships the same patch for the same
driver and applies it from its own manifest.

## Build

Inside the DevContainer (`.devcontainer/devcontainer.json`, image `espressif/idf:latest`):

```bash
cd /project
idf.py --preset korvo1 build      # -> build/korvo1/tinyml-demo-esp32s31.bin
idf.py --preset mosaico build     # -> build/mosaico/tinyml-demo-esp32s31.bin
```

Without a DevContainer, in the same image by hand:

```bash
docker run --rm -it -v "$PWD":/project -w /project espressif/idf:latest bash
idf.py --preset korvo1 build
```

`docker exec` **does not** inherit the image entrypoint's environment, so a build driven that way
needs the export first — otherwise it fails with `idf.py: command not found`:

```bash
docker exec -it tinyml-demo-esp32s31 bash -c \
  'source /opt/esp/idf/export.sh && idf.py --preset korvo1 build'
```

After editing a `sdkconfig.defaults`, re-run `idf.py --preset <devkit> reconfigure` before
building: `sdkconfig` is authoritative and defaults only fill values it does not already have.

## Flash

Both devkits have **16 MB** flash, and the partition table puts the app in an 8 MB `factory`
partition at `0x10000`, with a 6 MB `model` partition at `0x810000` for the ESP-SR models.

**Routine update — app only.** The model partition rarely changes:

```bash
esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/korvo1/tinyml-demo-esp32s31.bin
```

**Full flash** — a new board, a partition change, an IDF/toolchain change, or any change to
`CONFIG_SR_*`. The offsets are exactly what `idf.py build` writes into `build/<devkit>/flash_args`:

```bash
esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000  build/korvo1/bootloader/bootloader.bin \
  0x8000  build/korvo1/partition_table/partition-table.bin \
  0xd000  build/korvo1/ota_data_initial.bin \
  0x810000 build/korvo1/srmodels/srmodels.bin \
  0x10000 build/korvo1/tinyml-demo-esp32s31.bin
```

An app-only flash on top of a changed bootloader/toolchain boot-loops: the bootloader and the app
must come from the same build.

## Serial logs

```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 | tee tinyml-demo_$(date +%Y%m%d_%H%M%S).log
```

Ctrl-] exits miniterm. The application logs:

- a banner with the devkit name, chip, SDK version, free SRAM and free PSRAM
- one memory summary line per bring-up stage (`I2C`, `AUDIO`, `DISPLAY+LVGL`, `CAMERA`, `DETECTOR`, `VOICE`)
- one `CVT:` line per capture with the geometry the camera path produced
- one `INF:` line per inference with its duration and detection count

The ESP-Mosaico's camera module claims the USB Serial/JTAG pads, so its console is UART-only and
has to be read through a UART bridge rather than a USB port.
