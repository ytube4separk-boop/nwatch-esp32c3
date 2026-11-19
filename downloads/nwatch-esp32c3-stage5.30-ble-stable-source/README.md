# NWatch ESP32-C3 Stage 5.30 — BLE Stable

Firmware for the ESP32-C3 Super Mini with a 1.3-inch SH1106 I2C OLED, a DS3231 RTC and three buttons.

## Key Changes

- Uses NimBLE-Arduino 2.4.0. On the test board, NimBLE 2.5.1 caused the USB port to drop right after BLE initialization; 2.3.2 did not reset but could not start BLE advertising (`HCI 519`).
- BLE is initialized only after the boot UI has finished.
- The `CONNECT` menu has a Bluetooth ON/OFF toggle.
- On the Bluetooth screen, press the middle button to toggle ON/OFF; hold the middle button to go back.
- The Bluetooth state is stored in Preferences/NVS under the key `ble`, so it persists across power-off and reset.
- When Bluetooth is OFF, the BLE stack and radio are stopped, and BLE is not initialized on the next boot.
- When Bluetooth is ON, the device advertises as `NWatch C3` and supports ANCS and CTS for iPhone.

## Pin Connections

- OLED + DS3231 SDA: GPIO 0
- OLED + DS3231 SCL: GPIO 1
- Up button: GPIO 10
- Middle button: GPIO 20
- Down button: GPIO 21
- OLED and DS3231: 3V3 + GND

## Using Bluetooth

1. Go to `MAIN MENU` → `CONNECT`.
2. Press the middle button to switch between `BLUETOOTH: ON` and `BLUETOOTH: OFF`.
3. When ON, open Bluetooth on the iPhone and pair with `NWatch C3`.
4. Allow notification sharing when the iPhone asks. The watch syncs time via CTS and receives notifications via ANCS.

New devices default to Bluetooth ON. If OFF was selected previously, the firmware remembers that state after reset.

## ROM

- `nwatch-esp32c3-stage5.30-ble-stable.bin`: full ROM, flash at address `0x0`.
- `nwatch-esp32c3-stage5.30-ble-stable-app.bin`: application firmware only, flash at `0x10000` when the board already has the correct bootloader and partition table.

Flash settings: ESP32-C3, DIO, 40 MHz, 4 MB flash, 115200 baud.

## Test Results

- The boot log reaches `PAIR NWatch C3`, confirming BLE is actually advertising.
- USB port monitored for 150 seconds after BLE became active: only the initial `PRESENT` state was seen, with no port loss and no bootloop.
- Serial Monitor is configured with DTR/RTS set to 0. On the ESP32-C3 native USB, simply opening the monitor port may still trigger a single `USB_UART_CHIP_RESET`; this is not a bootloop.

## Build

Open this folder in VS Code + PlatformIO and run `PlatformIO: Build`. The app binary is generated at `.pio/build/esp32-c3-supermini/firmware.bin`.
