# Run

Environment and build tasks are managed by [mise](https://mise.jdx.dev).

## Setup

Trust the config once per clone, then provision ESP-IDF and the toolchain:

```shell
mise trust
mise run setup
```

`setup` uses `$IDF_PATH` when set (system install, CI image), otherwise clones
ESP-IDF v6.1 into `.idf/esp-idf`. It installs the esp32s3 toolchain, syncs the
`vendor/esp-usb` submodule and resolves managed components.

## Build and flash

```shell
mise run build
ESPPORT=/dev/cu.usbmodem1101 mise run flash
ESPPORT=/dev/cu.usbmodem1101 mise run monitor
```

Without `ESPPORT`, `idf.py` auto-detects the serial port.

## Release

```shell
mise run release
```

Produces `dist/qmx-waterfall-<git describe>.bin`: bootloader, partition table and
app merged at their flash offsets. Flash it to a blank device at offset 0:

```shell
esptool -b 460800 --before default-reset --after hard-reset --chip esp32s3 write-flash 0x0 dist/qmx-waterfall-<version>.bin
```

## Tasks

`mise tasks` lists everything: `setup`, `deps`, `build`, `clean`, `fullclean`,
`flash`, `monitor`, `size`, `release`.
