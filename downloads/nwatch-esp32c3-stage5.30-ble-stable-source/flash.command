#!/bin/zsh
set -e

SCRIPT_DIR="${0:A:h}"
PIO_PY="/Users/huyvector/.platformio/penv/bin/python"
ESPTOOL="/Users/huyvector/.platformio/packages/tool-esptoolpy/esptool.py"
ROM="$SCRIPT_DIR/nwatch-esp32c3-stage5.30-ble-stable.bin"

PORT=$(find /dev -maxdepth 1 -name 'cu.usbmodem*' -print | head -n 1)
if [[ -z "$PORT" ]]; then
  echo "Khong tim thay ESP32-C3. Giu BOOT, bam RESET, nha RESET, doi 2 giay roi nha BOOT."
  read "?Nhan Enter de dong..."
  exit 1
fi

echo "Nap NWatch Stage 5.30 BLE Stable qua $PORT"
"$PIO_PY" "$ESPTOOL" --chip esp32c3 --port "$PORT" --baud 115200 \
  --before usb_reset --after hard_reset --no-stub write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x0 "$ROM"

echo "Da nap xong. Cho boot khoang 10 giay; khong can mo Serial Monitor."
read "?Nhan Enter de dong..."
