# Dependencies

This project uses a mixed dependency model:

- ESP-IDF `v6.1`, checked out into `.idf/esp-idf` by `mise bootstrap` (no
  external ESP-IDF is used)
- ESP-IDF managed components from `components.espressif.com`
- esp-usb (USB host library + UAC driver) as the `vendor/esp-usb` git submodule

## esp-usb pin

- Path: `vendor/esp-usb` (git submodule: `https://github.com/espressif/esp-usb.git`)
- Current pin: `2e2032b` (esp-usb `master`, host `usb` 1.6.0, `usb_host_uac` 1.5.0)
- ESP-IDF: `v6.1`

The project resolves these components from the submodule via `main/idf_component.yml`:

- `espressif/usb` -> `../vendor/esp-usb/host/usb`
- `espressif/usb_host_uac` -> `../vendor/esp-usb/host/class/uac/usb_host_uac`

## Update process

1. Move the submodule to the new commit and stage it:
   - `git -C vendor/esp-usb fetch origin`
   - `git -C vendor/esp-usb checkout --detach <commit>`
2. Update the pin above.
3. Re-run dependency resolution: `mise run deps`.
4. Build (`mise run build`) and run unplug/replug regression checks.

## Quick check

Print current submodule commit:

```shell
git -C vendor/esp-usb rev-parse --short HEAD
```
