# QMX Waterfall

Firmware for an ESP32-S3 that turns a QRP Labs QMX into a standalone
waterfall display: it is a USB host, pulls the radio's USB audio (I/Q when the
radio is in IQ mode) and CAT status over the same cable, and renders a live
spectrum + waterfall on a 76×284 ST7789 panel with three buttons.

## Hardware

Reference build:

| Part | Detail |
| --- | --- |
| MCU | ESP32-S3 module, 16 MB flash, 8 MB octal PSRAM (`boards/n16r8`) |
| Panel | ST7789 / ST7789P3, 76×284, SPI, no MISO |
| Buttons | up / ok / down, active-low to GND |
| Radio | QRP Labs QMX on the ESP32-S3 USB host port |

Panel wiring (SPI2, native IOMUX; override per board in a profile):

| Signal | GPIO |
| --- | --- |
| SCL / CLK | 12 |
| SDA / MOSI | 11 |
| CS | 10 |
| DC | 9 |
| RST | 8 |
| BL (active-low) | 7 |

Buttons: up = 4, ok = 5, down = 6. The shutdown button defaults to GPIO 0
(`CONFIG_APP_QUIT_PIN`).

PSRAM is optional at runtime: without it the display falls back to a partial
internal draw buffer (`CONFIG_SPIRAM_IGNORE_NOTFOUND`), so the same image
boots on a board whose PSRAM is absent or fails to train.

## Install a release

Releases contain one factory image and one app image per board:

| File | Flash at | Effect |
| --- | --- | --- |
| `qmx-waterfall-<board>-<version>-factory.bin` | `0x0` | bootloader + partition table + app; **erases settings (NVS)** |
| `qmx-waterfall-<board>-<version>-app.bin` | `0x10000` | app only; keeps settings |
| `qmx-waterfall-<board>-<version>.sha256` | — | checksums for both |

```shell
esptool -b 460800 --before default-reset --after hard-reset \
  --chip esp32s3 write-flash 0x0 qmx-waterfall-n16r8-v0.1.0-factory.bin
```

Updating an existing install keeps the saved settings:

```shell
esptool -b 460800 --before default-reset --after hard-reset \
  --chip esp32s3 write-flash 0x10000 qmx-waterfall-n16r8-v0.1.0-app.bin
```

## Build from source

[mise](https://mise.jdx.dev) pins the toolchain; ESP-IDF v6.1 is provisioned
into `.idf/` on first bootstrap (see [RUN.md](RUN.md)):

```shell
mise trust
mise bootstrap
mise run build
ESPPORT=/dev/cu.usbmodem1101 mise run flash
ESPPORT=/dev/cu.usbmodem1101 mise run monitor
mise run test      # host tests, no board or ESP-IDF needed
```

`mise run release` builds the default board and writes the images above to
`dist/`.

## Board profiles

A board profile is a directory under `boards/` holding one
`sdkconfig.defaults` with the hardware: `CONFIG_IDF_TARGET`, flash, PSRAM,
panel and button pins (`boards/n16r8/sdkconfig.defaults`).

Every build uses exactly one profile — `sdkconfig.defaults` (project-wide
software settings) plus the profile's hardware defaults — and generates its own
`sdkconfig` under `build/<id>/`. The committed `sdkconfig` is never used, so a
clean clone builds the board the profile describes.

- `mise run build` / `flash` / `release` use the default board set in
  `mise.toml`; `QMX_BOARD=<id>` selects another.
- Adding a board: copy `boards/n16r8`, set `CONFIG_IDF_TARGET` and the
  hardware, and add it to the `firmware` matrix in `.github/workflows/ci.yml`
  once its image is verified. Only boards in that matrix are built by CI and
  published on a tag.
- The panel resolution is fixed in code (`main/ui/geometry.h`); a different
  panel size needs layout work, not just a new profile.

## Versioning and releases

There is no version-bump automation and no semantic-release: the release is
the git tag. `mise run version` prints `git describe` for the nearest
`vX.Y.Z` tag (`v0.1.0`, `v0.1.0-3-gabc1234`, plus `-dirty` for uncommitted
parent-repo changes), and that string names the artifacts.

CI builds every board in its matrix on every push; pushing a `vX.Y.Z` tag
also publishes the images and checksums as a GitHub release. Between tags, `mise run release` produces
`-<commit>`/`-dirty` images for testing that are never published.

## Documentation

- [RUN.md](RUN.md) — bootstrap, build, flash, release, tasks
- [docs/CAT.md](docs/CAT.md) — CAT link behaviour, IQ recovery, `AI1;` hazard
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) — ESP-IDF/esp-usb pins and patches
