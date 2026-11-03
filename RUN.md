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

1. installs the compilers for every `CONFIG_IDF_TARGET` used under `boards/` with
   `idf_tools.py install --targets=...` (no `install.sh`, no EIM; toolchains
   land in `IDF_TOOLS_PATH`, by default `~/.espressif/tools`)
2. creates the Python environment with `uv` at `.idf/venv` and installs IDF's
   `requirements.core.txt` against IDF's constraints file
   (`IDF_PYTHON_ENV_PATH`), instead of `idf_tools.py install-python-env`
3. syncs the `vendor/esp-usb` submodule and applies the local patches from
   `patches/`

Bootstrap does not modify tracked files: managed components are fetched by
the first build from the committed `dependencies.lock`, and re-resolving them
is a deliberate `mise run deps`.

`python` and `uv` are pinned in `mise.toml`, so the venv is reproducible across
machines. Re-running `mise bootstrap` is safe; `uv` and `idf_tools.py` keep
existing installs.

Everything ESP-IDF lives under `.idf/` (gitignored): the checkout and the uv
venv. mise sets `IDF_PATH` and `IDF_PYTHON_ENV_PATH` for this project, so an
activated ESP-IDF shell cannot change what the tasks use. Compilers are shared through `IDF_TOOLS_PATH`
(default `~/.espressif/tools`).

## Board profiles

A build always targets one profile: a directory under `boards/` holding a
`sdkconfig.defaults` with the hardware (`CONFIG_IDF_TARGET`, flash, PSRAM,
panel and button pins). There is no other board metadata.

`mise run idf -- <idf.py args...>` (`tasks/idf.sh`) is the one entry point to
`idf.py`; every other task wraps it. For the board in `QMX_BOARD` it runs

```
-B build/<id>
-D SDKCONFIG=build/<id>/sdkconfig
-D SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/<id>/sdkconfig.defaults
```

so each profile gets its own build directory and generated `sdkconfig`. The
project-root `sdkconfig` is not read; `sdkconfig.defaults` is the source of
truth and is committed.

- The default board is set in `mise.toml` (`QMX_BOARD`); `QMX_BOARD=<id>` in
  the shell selects another for any task.
- The boards CI builds and publishes are listed in the `firmware` matrix of
  `.github/workflows/ci.yml`.

`mise run release` also verifies that every line of the profile's
`sdkconfig.defaults` reached `build/<id>/sdkconfig`, so a Kconfig dependency
cannot silently override the hardware.

### Changing the configuration

Edit `build/<id>/sdkconfig` with `mise run menuconfig`, then fold the change
back into the committed defaults. `idf.py save-defconfig` always writes
`sdkconfig.defaults` in the project root, so split its output: software
settings stay in `sdkconfig.defaults`, hardware settings (flash size, PSRAM,
pins) go to `boards/<id>/sdkconfig.defaults`. Rebuild from scratch
(`mise run clean`) to confirm a clean clone produces the intended config.

## Build and flash

```shell
mise run build
ESPPORT=/dev/cu.usbmodem1101 mise run flash
ESPPORT=/dev/cu.usbmodem1101 mise run monitor
QMX_BOARD=<id> mise run build
```

Without `ESPPORT`, `idf.py` auto-detects the serial port. Builds land in
`build/<id>/`.

## Release

```shell
mise run release            # default profile
QMX_BOARD=<id> mise run release
```

`mise run release` builds, merges bootloader/partition table/app, and writes:

| File | Flash at | Notes |
| --- | --- | --- |
| `dist/qmx-waterfall-<board>-<version>-factory.bin` | `0x0` | erases NVS (settings) |
| `dist/qmx-waterfall-<board>-<version>-app.bin` | `0x10000` | keeps settings |
| `dist/qmx-waterfall-<board>-<version>.sha256` | — | checksums |

`<version>` comes from `mise run version`: `git describe` against `vX.Y.Z`
tags, `-dirty` only for uncommitted parent-repo changes (the vendored submodule
patches do not count). A tag build must produce exactly the tag name, which the
CI enforces before building a tag.

## Tasks

| Task | Does |
| --- | --- |
| `bootstrap` | provision ESP-IDF, toolchains, venv, submodules |
| `deps` | re-resolve managed components into `dependencies.lock` |
| `build`, `flash`, `monitor`, `menuconfig`, `clean` | the selected board |
| `idf -- <args>` | any other `idf.py` command for the selected board (e.g. `size`) |
| `release` | build + verify the profile + write images to `dist/` |
| `version` | the version string used in artifact names |
| `test` | host-only suite (`test/run.sh`, ASan/UBSan); no ESP-IDF or board |

Layout: `tasks/setup.toml` (bootstrap, deps, the hidden `vendor-patches` that
`idf` depends on), `tasks/build.toml` (idf.py wrappers), `tasks/release.toml`
(release, version), `tasks/idf.sh` (board selection and ESP-IDF activation),
and `test` in `mise.toml`.
