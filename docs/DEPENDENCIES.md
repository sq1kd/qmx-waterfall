# Dependencies

This project uses a mixed dependency model:

- ESP-IDF managed components from `components.espressif.com`
- Vendored USB stack from `vendor/esp-usb`

## Vendored esp-usb pin

- Path: `vendor/esp-usb`
- Current pin: `78c87b7`

The project resolves these components from the vendored tree via `main/idf_component.yml`:

- `espressif/usb` -> `../vendor/esp-usb/host/usb`
- `espressif/usb_host_uac` -> `../vendor/esp-usb/host/class/uac/usb_host_uac`

## Update process

1. Update vendored repo commit in `vendor/esp-usb`.
2. Re-run dependency resolution:
   - `idf.py update-dependencies`
   - `idf.py reconfigure`
3. Build and run unplug/replug regression checks.

## Quick check

Print current vendored commit:

```shell
git -C vendor/esp-usb rev-parse --short HEAD
```
