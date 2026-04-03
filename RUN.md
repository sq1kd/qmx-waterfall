# Run

```shell
esptool -b 460800 --before default-reset --after hard-reset --chip esp32s3 write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 usb_host_lib.bin

idf-monitor -b 115200 --toolchain-prefix xtensa-esp32s3-elf- --target esp32s3 .\usb_host_lib.elf
```
