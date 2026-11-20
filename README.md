# XIAO nRF54LM20A BTHome Button

Zephyr/PlatformIO port of the two-button BTHome v2 firmware for the Seeed
Studio XIAO nRF54LM20A. The original nRF52840 project remains separate.

## Hardware assumptions

- Board: Seeed Studio XIAO nRF54LM20A or XIAO nRF54LM20A Sense.
- Fit and connect the external Bluetooth antenna.
- Connect normally-open switches from D1 and D2 to GND. The firmware enables
  the internal pull-ups; D1 wins if both wake simultaneously.
- Connect a supported single-cell LiPo battery to BAT+/BAT-. Battery voltage
  is read from the onboard nPM1300 charger sensor.

The board pin map routes D1 to P1.31 and D2 to P1.30. The current battery
percentage estimate uses the same 3.3–4.2 V linear mapping as the nRF52840
version; it is a rough indicator, not a fuel-gauge percentage.

## Behavior

- Single, double, triple, and long presses are sent as BTHome v2 button events.
- Each button advertises a distinct name and static BLE identity.
- BLE transmit power is configured to +8 dBm for the external antenna.
- BTHome device-info byte `0x44` marks the device as trigger-based.
- Every event includes a per-button BTHome packet ID, persisted in the
  XIAO's internal storage so repeated identical gestures are distinguishable
  after System OFF reboots.
- The battery object is included when the nPM1300 voltage reading succeeds.
  If it fails, the button event is still advertised and an error is logged.
- After advertising, the device enters System OFF and wakes from either input.

## Build and flash

Install PlatformIO Core and the Seeed Studio board platform, then run:

```sh
pio run -e seeed-xiao-nrf54lm20a
pio run -e seeed-xiao-nrf54lm20a --target upload
```

### Linux upload permissions

The board exposes CMSIS-DAP as both a USB device and a HID device (USB ID
`2886:0068`). OpenOCD needs access to the HID node as well as the USB device.
If it cannot open the CMSIS-DAP device, add these udev rules:

```text
SUBSYSTEM=="usb", ATTR{idVendor}=="2886", ATTR{idProduct}=="0068", MODE="0660", GROUP="dialout", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2886", ATTRS{idProduct}=="0068", MODE="0660", GROUP="dialout", TAG+="uaccess"
```

Create and load the rule file with:

```sh
sudo tee /etc/udev/rules.d/60-seeed-cmsis-dap.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="2886", ATTR{idProduct}=="0068", MODE="0660", GROUP="dialout", TAG+="uaccess"
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2886", ATTRS{idProduct}=="0068", MODE="0660", GROUP="dialout", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules
```

Then unplug and reconnect the board.

The logged-in user must be a member of `dialout` for the group permission to
apply. Check that the matching `/dev/hidraw*` node is group-writable, then retry
the upload command.

The project uses the Zephyr framework as required by the nRF54LM20A board;
the nRF52840 Arduino/Bluefruit code cannot be reused directly. The ARM
GCC 12.3 toolchain is pinned because the older compiler selected by the
current Seeed platform fails to compile its Zephyr Bluetooth controller.

## References

- [XIAO nRF54LM20A getting started](https://wiki.seeedstudio.com/xiao_nrf54lm20a_getting_started/)
- [Seeed PlatformIO battery example](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/examples/zephyr-battery)
- [Seeed PlatformIO low-power example](https://github.com/Seeed-Studio/platform-seeedboards/tree/main/examples/zephyr-lowpower)
