# Run

Environment and build tasks are managed by [mise](https://mise.jdx.dev).

## Setup

Trust the config once per clone, then provision ESP-IDF and the toolchain:

```shell
mise trust
mise run setup
```

`setup` uses `$IDF_PATH` when set (system install, CI image), otherwise clones
ESP-IDF v6.1 into `.idf/esp-idf`. It then:

1. installs the esp32s3 compilers with `idf_tools.py install --targets=esp32s3`
   (no `install.sh`, no EIM; toolchains land in `IDF_TOOLS_PATH`, by default
   `~/.espressif/tools`)
2. creates the Python environment with `uv` at `.idf/venv` and installs IDF's
   `requirements.core.txt` against IDF's constraints file
   (`IDF_PYTHON_ENV_PATH`), instead of `idf_tools.py install-python-env`
3. syncs the `vendor/esp-usb` submodule and resolves managed components

`python` and `uv` are pinned in `mise.toml`, so the venv is reproducible across
machines. Re-running `setup` is safe; `uv` and `idf_tools.py` keep existing
installs.

mise sets `IDF_PATH`, `IDF_PYTHON_ENV_PATH` and `IDF_TARGET` for this project,
so an activated ESP-IDF shell does not change which checkout or venv the tasks
use. `setup` refuses to create a Python environment outside `.idf/`; toolchains
are shared through `IDF_TOOLS_PATH` (default `~/.espressif/tools`).

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
