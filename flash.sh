#!/bin/bash
# IH-2000 Flash Script
# Flash the firmware to ESP32 via esptool

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR/firmware"

# Default serial port
PORT="${1:-/dev/ttyUSB0}"
BAUD=921600

echo "========================================"
echo "  IH-2000 Induction Heater Flasher"
echo "========================================"
echo ""

if [ ! -f "$FIRMWARE_DIR/IH-2000-v1.0.bin" ]; then
    echo "[ERROR] Firmware not found at: $FIRMWARE_DIR"
    echo ""
    echo "Please build the firmware first:"
    echo "  1. From GitHub Actions: download the artifact"
    echo "  2. Place binaries in: $FIRMWARE_DIR"
    echo ""
    echo "Required files:"
    echo "  - IH-2000-v1.0.bin"
    echo "  - bootloader.bin"
    echo "  - partitions.bin"
    exit 1
fi

echo "[INFO] Serial port: $PORT"
echo "[INFO] Baud rate: $BAUD"
echo ""
echo "[FLASH] Bootloader  → 0x1000"
$ESPTOOL_PATH esptool.py --chip esp32 --port "$PORT" --baud $BAUD \
    --before default_reset --after hard_reset write_flash -z --flash_mode dio \
    --flash_freq 40m --flash_size detect \
    0x1000 "$FIRMWARE_DIR/bootloader.bin" \
    0x8000 "$FIRMWARE_DIR/partitions.bin" \
    0x10000 "$FIRMWARE_DIR/IH-2000-v1.0.bin"

if [ $? -eq 0 ]; then
    echo ""
    echo "[SUCCESS] Firmware flashed successfully!"
    echo "[INFO] The ESP32 will reboot automatically."
    echo "[INFO] Connect to WiFi: SSID 'IH-2000', password 'induction2000'"
else
    echo ""
    echo "[ERROR] Flashing failed!"
    echo "Check the serial port and try again."    
    exit 1
fi
