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

Both devkits have **16 MB** flash. The layout is one CSV per devkit, `devkits/<devkit>/partitions.csv`,
and the two differ by a single line — the ESP-Mosaico adds a `coredump` partition:

```csv
# devkits/korvo1/partitions.csv — ESP32-S31 Korvo-1
# Name,   Type, SubType, Offset,  Size,     Flags
nvs,      data, nvs,     0x9000,  0x4000,
otadata,  data, ota,     0xd000,  0x2000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 8M,
model,    data, spiffs,  ,        6M,
```

```csv
# devkits/mosaico/partitions.csv — the same five rows, plus:
coredump, data, coredump, 0xE10000, 64K,
```

| Partition | Offset | Size | Holds | Which command writes it |
|---|---|---|---|---|
| `nvs` | `0x9000` | 16 KB | NVS key/value store — **unused by this app**: nothing calls `nvs_flash_init()` | — |
| `otadata` | `0xd000` | 8 KB | which slot to boot — always `factory` here, there are no OTA slots | full `flash` |
| `phy_init` | `0xf000` | 4 KB | RF calibration data | full `flash` / first boot |
| **`factory`** | `0x10000` | **8 MB** | **the application image** — 7.1 MB on the Korvo-1, 7.3 MB on the ESP-Mosaico, because the YOLO11n model is linked into it, not stored separately | `app-flash` |
| **`model`** | `0x810000` | **6 MB** | **`srmodels.bin`** — the ESP-SR WakeNet/MultiNet models, ~3.0 MB used, mounted as SPIFFS and read by `esp_srmodel_init("model")` | full `flash` |
| `coredump` | `0xE10000` | 64 KB | panic dumps — ESP-Mosaico only | — |

### What to flash, and when

| What changed | Command | Why |
|---|---|---|
| application code | `app-flash` | only `factory` differs — ~75 s for 7.1 MB |
| `CONFIG_SR_*` (a different wake word or command model), or `partitions.csv` | full `flash` | `srmodels.bin` is a *new* file; a stale one left on the chip makes the board panic at boot while the model is parsed |
| the IDF version/toolchain, or anything that changes the bootloader | full `flash` | bootloader and app must come from the same build; an app-only flash on top of a changed bootloader boot-loops |
| a board that has never been flashed, or after `erase-flash` | full `flash` | there is nothing to preserve |
| nothing — you only want the console | `monitor` | |

**Why the model partition is usually skipped.** `idf.py flash` passes `--skip-flashed`, so it writes
only the partitions whose image differs from what is already on the chip. A routine build that
changed nothing but application code therefore does **not** spend 43 s rewriting the 3 MB model
partition. Raw `esptool write-flash` has no such check: it writes every image in the list, every
time. That is the practical reason to prefer `idf.py flash`, and to keep the `esptool` form for
machines that have no IDF.

Measured on the Korvo-1 over its CP2102N bridge at `-b 921600`: the model partition, 3,052,231 B in
43 s; the app, 7,137,328 B in 74 s. The ESP-Mosaico's USB-OTG path is slower — the same class of app
image took 153 s.

**Preferred: let `idf.py` do it**, from inside the container with the devkit's USB port passed
through (`--device=/dev/ttyUSB0`, or in the DevContainer's `runArgs`):

```bash
idf.py --preset korvo1 -p /dev/ttyUSB0 flash       # the whole set the build produced
idf.py --preset korvo1 -p /dev/ttyUSB0 app-flash   # app partition only
idf.py --preset korvo1 -p /dev/ttyUSB0 monitor     # console + panic decoding
```

`flash` writes bootloader, partition table, `ota_data`, the ESP-SR models and the app at the offsets
in `build/<devkit>/flash_args`. `app-flash` writes only `0x10000`.

**The same thing with `esptool` alone** — for a machine that has `esptool` but no IDF (a flash
runner, a CI box). These are the offsets `idf.py build` prints; the flags come from the same
`flash_args`:

```bash
# app only — the routine update
esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x10000 build/korvo1/tinyml-demo-esp32s31.bin

# full set — new board, partition change, IDF/toolchain change, or any CONFIG_SR_* change
esptool --chip esp32s31 -p /dev/ttyUSB0 -b 921600 \
  --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000  build/korvo1/bootloader/bootloader.bin \
  0x8000  build/korvo1/partition_table/partition-table.bin \
  0xd000  build/korvo1/ota_data_initial.bin \
  0x810000 build/korvo1/srmodels/srmodels.bin \
  0x10000 build/korvo1/tinyml-demo-esp32s31.bin
```

Leaving out `srmodels.bin` from that list is the `app-flash` case: correct as long as the model
partition already holds the models the current `sdkconfig` selects.

## Serial logs

```bash
python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 | tee tinyml-demo_$(date +%Y%m%d_%H%M%S).log
```

Ctrl-] exits miniterm. Inside the container, `idf.py --preset <devkit> -p /dev/ttyUSB0 monitor` does
the same job and additionally hands the ELF to the panic decoder. The application logs:

- a banner with the devkit name, chip, SDK version, free SRAM and free PSRAM
- one memory summary line per bring-up stage (`I2C`, `AUDIO`, `DISPLAY+LVGL`, `CAMERA`, `DETECTOR`, `VOICE`)
- one `CVT:` line per capture with the geometry the camera path produced
- one `INF:` line per inference with its duration and detection count

The ESP-Mosaico's camera module claims the USB Serial/JTAG pads, so its console is UART-only and
has to be read through a UART bridge rather than a USB port.
