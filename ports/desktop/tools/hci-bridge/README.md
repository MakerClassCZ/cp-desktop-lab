# A Bluetooth controller on USB for `--ble PORT`

The desktop port's `_bleio` is the HCI host stack the AirLift boards use. With `--ble PORT` it talks
HCI (H4, as on a UART) to a real Bluetooth controller on a serial port of the computer, so a
program in the lab advertises to and serves a real phone, and every packet is in the trace and,
with `--ble-log PATH`, in a btsnoop file for Wireshark. Two boards make good controllers.

## A Pico 2 W or a Pico W

`pico/` is a bridge between the board's USB serial port and its CYW43439's Bluetooth core, on the
Pico SDK. The release has it built: `hci-bridge-pico2w.uf2` for a Pico 2 W, `hci-bridge-picow.uf2`
for a Pico W (the same chip on an RP2040).

1. Hold BOOTSEL, plug the board in, copy its `.uf2` to the `RP2350` (`RPI-RP2`) drive. Its LED
   lights when the Bluetooth core is up, and it is a serial port named "Pico HCI bridge" (USB
   2E8A:0009 on a Pico 2 W, 2E8A:000A on a Pico W). A 1200-baud open of the port puts it back in
   the bootloader.
2. `circuitpython --ble COM7` (`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem…` on macOS).

Its address is the board's own, from the chip (88:A2:9E:… on a Raspberry Pi board). The bridge
reads the core only when the driver says it has packets: reading it without a pause crashes it.

To build it (Linux or WSL, `cmake`, an ARM GCC; the Pico SDK is a submodule of this repository):

```
git submodule update --init ports/raspberrypi/sdk ports/raspberrypi/lib/cyw43-driver ports/raspberrypi/lib/lwip lib/tinyusb
export PICO_SDK_PATH=$PWD/ports/raspberrypi/sdk PICO_TINYUSB_PATH=$PWD/lib/tinyusb
export PICO_CYW43_DRIVER_PATH=$PWD/ports/raspberrypi/lib/cyw43-driver PICO_LWIP_PATH=$PWD/ports/raspberrypi/lib/lwip
cmake -S ports/desktop/tools/hci-bridge/pico -B build-hci-bridge && make -C build-hci-bridge
```

`-DPICO_BOARD=pico_w` on the `cmake` line builds it for a Pico W.

## An nRF52840 board with Zephyr's `hci_uart`

Zephyr's `samples/bluetooth/hci_uart` is such a controller as it is. For an Adafruit Feather
nRF52840 (or another nRF52840 board with the Adafruit UF2 bootloader and SoftDevice S140 6.1.1):

```
west build -b adafruit_feather_nrf52840/nrf52840/uf2 zephyr/samples/bluetooth/hci_uart \
    -- -DEXTRA_CONF_FILE=$PWD/ports/desktop/tools/hci-bridge/zephyr-nrf52840.conf
```

`zephyr-nrf52840.conf` links the program after the bootloader's MBR and SoftDevice. Without it the
UF2 starts at 0x0, and copying it would overwrite them: check that the UF2's first block is at
0x26000 before copying it to the `FTHR840BOOT` drive. The board is then a serial port named
"Feather HCI bridge" (USB 2FE3:0004). Its controller has no public address: the host takes the
chip's static random one (EC:AC:… on the one tried), the same at every start.

Either way, a board that opens its serial ports itself, such as a board manager, holds the port:
let it go first.
