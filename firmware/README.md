## Fri3d Camp 2026 Communicator add-on firmware

This firmware runs on the Fri3d camp 2026 communicator add-on board, which is powered by [CH32X035](https://www.wch-ic.com/products/CH32X035.html) MCU, the same part that can be found as a coprocessor on the Fri3d badge 2026. More information about the 2026 communicator addon can be found [here](https://fri3dcamp.github.io/badge_2026/en/communicator/) ([dutch](https://fri3dcamp.github.io/badge_2026/communicator)). This MCU only controls the keyboard part of the communicator. The audio related functions of the communicator are handled by the main MCU of Fri3d badge through the expansion connector.

The firmware outputs [HID report packets](https://files.microscan.com/helpfiles/ms4_help_file/ms-4_help-02-46.html) (8 bytes) on USB and UART on the expansion connector. So the 2026 communicator addon can be used standalone using the USB connector attached to a PC for use as a normal keyboard, or in combination with the Fri3d badge [2024](https://github.com/Fri3dCamp/badge_2024) or [2026](https://github.com/Fri3dCamp/badge_2026) using the expansion connector.

## HID Report packets

A HID report packet ([Protocol 1](https://www.usb.org/sites/default/files/hid1_11.pdf)) consists of 8 bytes. The first byte indicates the modifier keys that have been pressed:

| Bit | Modifier Key |
|-|-|
| 7 | RIGHT GUI |
| 6 | RIGHT ALT |
| 5 | RIGHT SHIFT |
| 4 | RIGHT CTRL |
| 3 | LEFT GUI |
| 2 | LEFT ALT |
| 1 | LEFT SHIFT |
| 0 | LEFT CTRL |

The second byte is reserved, the remaining 6 bytes can contain a [HID keycode](https://gist.github.com/MightyPork/6da26e382a7ad91b5496ee55fdc73db2).

On the 2026 communicator, UART RX is also connected to the expansion connector. This means the badge firmware can send a byte to control the backlight of the 2026 communicator. The byte should set a value between 0 and 100.

### I2C

The Fri3d badge [2024](https://github.com/Fri3dCamp/badge_2024) and [2026](https://github.com/Fri3dCamp/badge_2026) can also communicate with the 2026 communicator add-on through I2C (address ```0x40```). The following registers can be used to interface/control with the add-on:

| Register | Name | Access | Bytes | description |
|-|-|-|-|-|
| 0x00 | Version number | R | 3 | Reports the firmware version number |
| 0x03 | current HID report packet | R | 8 | An 8-byte HID report packet (see above) |
| 0x0b | Configuration | R/W | 1 | a 1-byte configuration register (see below) |
| 0x0c | Backlight | R/W | 2 | Keyboard backlight intensity (0-100) |

The configuration is a 1-byte value with the following encoding:
| Bit | Name |
|-|-|
| \[7:2\] | reserved |
| 1 | reboot to bootloader |
| 0 | enable interrupt mode (not implemented yet) |

## Building

Use [platformio](https://platformio.org) to build this project. You should install the [ch32v platform package](https://github.com/Community-PIO-CH32V/platform-ch32v) as well. If you use the command line, build using:

```
pio run
```

## Flashing

### Through USB

To flash your device, unplug the USB cable, press and hold the boot button on the board while plugging in the USB cable again. Then upload using the command:

```
pio run -t upload
```

It will use [wchisp](https://github.com/Community-PIO-CH32V/tool-wchisp) to flash the binary to the [CH32X035](https://www.wch-ic.com/products/CH32X035.html) chip.

### Using your badge

Unlike the 2024 communicator, the 2026 communicator can be reflashed by your badge.
TODO: link to flashing instructions (micropython app?)

## Usage

The keyboard presents itself as a HID input device.
The ```Fn``` key can be used to trigger special functions:
 * ```Fn+Backspace```: Delete
 * ```Fn+Up```: Page Up
 * ```Fn+Down```: Page Down
 * ```Fn+Left```: Home
 * ```Fn+Right```: End
 * ```Fn+Spacebar```: Toggle keyboard backlight
 * ```Fn+Right Shift```: Toggle Caps Lock
