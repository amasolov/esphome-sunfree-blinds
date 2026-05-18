# Wiring: ESP32-S3 + CC1101

## Pin connections

| Signal   | ESP32-S3 GPIO | CC1101 Module Pin | Notes                          |
|----------|---------------|-------------------|--------------------------------|
| SPI CLK  | GPIO12        | SCLK (SCK)        | SPI clock                      |
| SPI MOSI | GPIO11        | MOSI (SI)          | ESP32 to CC1101 data           |
| SPI MISO | GPIO13        | MISO (SO)          | CC1101 to ESP32 data           |
| CS       | GPIO5         | CSn (SS)           | Chip select, active low        |
| GDO0     | GPIO4         | GDO0               | Packet RX interrupt + async TX |
| 3.3V     | 3V3           | VCC                | **3.3V only** (not 5V tolerant)|
| GND      | GND           | GND                |                                |

GDO2 is not connected (active-high interrupt not used in this design).

## Wiring diagram

```
  ESP32-S3 DevKitC-1                  CC1101 Module
  +-----------------+                 +----------+
  |             3V3 |----[3.3V]------>| VCC      |
  |             GND |----[GND]------->| GND      |
  |          GPIO12 |----[CLK]------->| SCLK     |
  |          GPIO11 |----[MOSI]------>| MOSI     |
  |          GPIO13 |<---[MISO]------| MISO     |
  |           GPIO5 |----[CS]-------->| CSn      |
  |           GPIO4 |<-->[GDO0]----->| GDO0     |
  |                 |                 |          |
  |          GPIO48 |--[onboard LED] |  [ANT]~~~|  antenna
  +-----------------+                 +----------+
```

## Board compatibility

The reference build uses an **ESP32-S3-DevKitC-1**, but any ESP32 variant
with hardware SPI will work. Adjust the GPIO numbers in the YAML to match
your board. The component uses:

- Hardware SPI (CLK, MOSI, MISO, CS)
- One GPIO for GDO0 (packet interrupt in RX, async serial data in TX)

Boards known to work:

| Board             | Notes                                    |
|-------------------|------------------------------------------|
| ESP32-S3-DevKitC-1| Reference build, pins as listed above    |
| ESP32-DevKitC     | Use any free GPIOs, update YAML pins     |
| ESP32-C3          | Single-core, should work but untested    |

## CC1101 module variants

Most breakout boards labelled "CC1101 433MHz" will work. Common variants:

- **CC1101 basic module** (green PCB, 8-pin header): standard, recommended.
  Typical TX power is +10 dBm. Comes with a coil antenna or SMA connector.

- **CC1101 + PA/LNA** (with CC2591 or similar): has an external power
  amplifier. Higher TX power (~+20 dBm) and better RX sensitivity, but
  may be overkill for in-home use. Can interfere with nearby devices if
  TX power is too high.

For blinds within the same room or adjacent rooms, the basic module is
sufficient. The Sunfree motor's WOR (wake-on-radio) is designed for
the CMT2300A's modest TX power, so a basic CC1101 module provides
comparable range.

## Antenna

- Use the antenna that comes with the CC1101 module (coil or wire).
- If using an SMA connector module, attach a 433 MHz antenna.
- A 17.3 cm straight wire soldered to the ANT pad works as a quarter-wave
  antenna for 433 MHz.
- Keep the antenna away from metal surfaces and the ESP32's WiFi antenna.

## Power supply

- The CC1101 operates at **3.3V only**. Do not connect VCC to 5V.
- During TX (especially long WOR preamble bursts), the CC1101 draws up to
  ~30 mA. Ensure your 3.3V supply can handle the combined ESP32 + CC1101
  load. Most USB-powered dev boards are fine.
- If you experience TX reliability issues, try adding a 100 uF capacitor
  between VCC and GND near the CC1101 module.
