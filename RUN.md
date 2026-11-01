# Run

Environment and build tasks are managed by [mise](https://mise.jdx.dev).

## Bootstrap

Trust the config once per clone, then provision ESP-IDF and the toolchain:

```shell
mise trust
mise bootstrap
```

`mise bootstrap` installs the tools pinned in `mise.toml` and runs the
`bootstrap` task. The task clones ESP-IDF v6.1 into `.idf/esp-idf` (or checks
out that ref if the directory is present at another version). There is no
external ESP-IDF: the scripts ignore `IDF_PATH` from your shell, so local and CI
builds use the same tree. It then:

1. installs the esp32s3 compilers with `idf_tools.py install --targets=esp32s3`
   (no `install.sh`, no EIM; toolchains land in `IDF_TOOLS_PATH`, by default
   `~/.espressif/tools`)
2. creates the Python environment with `uv` at `.idf/venv` and installs IDF's
   `requirements.core.txt` against IDF's constraints file
   (`IDF_PYTHON_ENV_PATH`), instead of `idf_tools.py install-python-env`
3. syncs the `vendor/esp-usb` submodule and resolves managed components

`python` and `uv` are pinned in `mise.toml`, so the venv is reproducible across
machines. Re-running `mise bootstrap` is safe; `uv` and `idf_tools.py` keep
existing installs.

Everything ESP-IDF lives under `.idf/` (gitignored): the checkout and the uv
venv. mise sets `IDF_PATH`, `IDF_PYTHON_ENV_PATH` and `IDF_TARGET` for this
project, and `tools/idf-env.sh` re-asserts the same paths, so an activated
ESP-IDF shell cannot change what the tasks use. Compilers are shared through
`IDF_TOOLS_PATH` (default `~/.espressif/tools`).

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

`mise tasks` lists everything: `bootstrap`, `deps`, `build`, `clean`, `fullclean`,
`flash`, `monitor`, `size`, `release`.
