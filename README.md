# esphome-sunfree-blinds

ESPHome custom component for **Sunfree AC2001-15D** 433 MHz motorized blinds.
Replaces the Tuya AC801B cloud hub with a fully local **ESP32 + CC1101** bridge.

## Features

- **Open / Close / Stop / Set Position** with precise percentage control
- **Battery state of charge** reported as a Home Assistant sensor
- **Dual-roller support** -- each Sunfree unit has independent day and night
  rollers, exposed as separate cover entities
- **Wake-on-Radio (WOR)** -- 800-byte preamble wakes sleeping motors, matching
  the original hub's behavior
- **No cloud dependency** -- direct 433 MHz RF communication, no internet required
- **Coexistence** -- can run alongside the original Tuya hub (same hub ID) or
  replace it entirely

## Hardware

| Component | Description |
|-----------|-------------|
| ESP32 dev board | ESP32-S3-DevKitC-1 (reference) or any ESP32 with SPI |
| CC1101 module | 433 MHz variant with antenna |
| Sunfree AC2001-15D | 433 MHz motorized blind (sold under various brands) |

### Wiring (ESP32-S3)

| Signal   | GPIO   | CC1101 Pin |
|----------|--------|------------|
| SPI CLK  | GPIO12 | SCLK       |
| SPI MOSI | GPIO11 | MOSI       |
| SPI MISO | GPIO13 | MISO       |
| CS       | GPIO5  | CSn        |
| GDO0     | GPIO4  | GDO0       |
| 3.3V     | 3V3    | VCC        |
| GND      | GND    | GND        |

See [docs/WIRING.md](docs/WIRING.md) for a detailed diagram, board
compatibility notes, and antenna guidance.

## Quick start

### 1. Install the component

Add this to your ESPHome YAML:

```yaml
external_components:
  - source: {type: git, url: https://github.com/YOUR_USER/esphome-sunfree-blinds}
    components: [sunfree_blinds]
```

Or clone the repo and use a local path:

```yaml
external_components:
  - source: {type: local, path: /path/to/esphome-sunfree-blinds/components}
    components: [sunfree_blinds]
```

### 2. Configure the radio

```yaml
spi:
  clk_pin: GPIO12
  mosi_pin: GPIO11
  miso_pin: GPIO13

cc1101:
  id: cc1101_radio
  cs_pin: GPIO5
  gdo0_pin: GPIO4
  frequency: 433.95MHz
  modulation_type: GFSK
  symbol_rate: 40000
  fsk_deviation: 38kHz
  filter_bandwidth: 203kHz
  packet_mode: true
  packet_length: 29
  sync_mode: "16/16"
  sync1: 0xD4
  sync0: 0x92
  num_preamble: 7
  on_packet:
    then:
      - lambda: |-
          id(sunfree_hub).on_cc1101_packet(x, rssi);
```

### 3. Configure the hub and motors

```yaml
sunfree_blinds:
  id: sunfree_hub
  hub_id: "15241342"        # your hub's 4-byte ID (8 hex chars)
  cc1101_id: cc1101_radio

cover:
  - platform: sunfree_blinds
    name: "Office Night Blind"
    motor_id: "04250e5b"    # night roller motor ID
    battery:
      name: "Office Night Battery"

  - platform: sunfree_blinds
    name: "Office Day Blind"
    motor_id: "05250e5b"    # day roller motor ID
    battery:
      name: "Office Day Battery"
```

See [example.yaml](example.yaml) for a complete working configuration.

## Finding your hub ID and motor IDs

### Migrating from a Tuya AC801B hub

If your blinds are already paired to a Tuya hub, you need its **hub ID**
and each motor's **motor ID** to control them from the ESP32 without
re-pairing.

**Option A: SWD debug dump** (requires hardware debugger)

The hub stores the device table in SRAM. Connect a debugger (e.g. ST-Link)
to the hub's MCU (HDSC HC32L170) and read the memory at `0x2000080C`.
The hub ID is at the start, followed by pairs of motor IDs (night + day
for each physical unit).

**Option B: Sniff RF traffic**

Flash this component with any placeholder `hub_id`, enable DEBUG logging,
and trigger the Tuya hub to send a command (open/close a blind from the
Tuya app). The ESP32 will log overheard commands including the hub ID and
motor IDs:

```
[sunfree] RX CMD h=15241342 m=04250e5b a=4 v=100 s=0x30
```

Here `h=15241342` is the hub ID and `m=04250e5b` is the motor ID.

### Dual-roller motor IDs

Each Sunfree AC2001-15D unit registers **two** motor IDs when paired:

- **Night roller** (roller A): lower first byte
- **Day roller** (roller B): higher first byte (differs by 1)

Example: night = `04250e5b`, day = `05250e5b`.

### Pairing new motors (not yet implemented)

Pairing new motors directly from the ESP32 (without a Tuya hub) is
documented in [docs/PROTOCOL.md](docs/PROTOCOL.md) but not yet
implemented in the component. Contributions welcome.

## Configuration reference

### Hub (`sunfree_blinds`)

| Key | Required | Description |
|-----|----------|-------------|
| `id` | Yes | ESPHome component ID |
| `hub_id` | Yes | 4-byte hub identifier (8 hex characters) |
| `cc1101_id` | Yes | ID of the CC1101 component |

### Cover (`platform: sunfree_blinds`)

| Key | Required | Description |
|-----|----------|-------------|
| `name` | Yes | Entity name in Home Assistant |
| `motor_id` | Yes | 4-byte motor identifier (8 hex characters) |
| `battery` | No | Sub-schema for battery sensor (sensor config) |

## How it works

The component reverse-engineers the Sunfree/Tuya 433 MHz protocol:

1. **XXTEA encryption** with a shared key (`SUNFRE20201625`)
2. **GFSK modulation** at 40 kbps on 433.920 MHz
3. **CRC-8/MAXIM** checksum on every packet
4. **800-byte WOR preamble** to wake battery-powered motors from sleep
5. **Async serial bit-banging** on GDO0 for gap-free preamble transmission

The CC1101 handles the radio layer. The ESP32 handles encryption,
packet framing, and the WOR preamble via direct GDO0 control.

For full protocol details, see [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Troubleshooting

### Motor does not respond

- Verify `hub_id` and `motor_id` are correct (8 hex chars each).
- Check that the CC1101 module is wired correctly (SPI + GDO0).
- Ensure the CC1101 module is a **433 MHz** variant (not 868/915).
- Try moving the ESP32+CC1101 closer to the motor.

### Motor responds intermittently

- The CC1101 crystal frequency may differ slightly from the motor's
  CMT2300A. If your CC1101 has a measurable offset, you can compensate
  by adjusting the TX frequency in `sunfree_hub.h` (search for
  `set_frequency(433933000.0f)` -- the 433933 accounts for a -13 kHz
  crystal offset on the reference board).

### Position values seem inverted

- The Sunfree motor uses 0 = fully open, 100 = fully closed.
- Home Assistant uses 0 = closed, 1.0 = open.
- The component handles this conversion automatically. If positions
  appear wrong, check that you are not double-inverting in automations.

### Battery percentage not updating

- Battery is reported in the motor's status packets, which are sent
  after each command execution. Trigger an open/close/stop to receive
  a fresh status report.

## Project structure

```
components/
  sunfree_blinds/
    __init__.py          ESPHome hub platform (YAML config -> C++ codegen)
    cover.py             ESPHome cover platform (per-motor config)
    sunfree_protocol.h   Encryption, packet framing, CRC-8, parsers
    sunfree_hub.h        CC1101 radio control, WOR preamble, piggyback
    sunfree_cover.h      Cover entity, position mapping, status handling
example.yaml             Complete working ESPHome configuration
docs/
  PROTOCOL.md            Full RF protocol specification
  WIRING.md              Hardware wiring guide
```

## License

MIT License. See [LICENSE](LICENSE).

## Credits

Protocol reverse-engineered from the Tuya AC801B hub firmware
(HDSC HC32L170 MCU + CMT2300A radio). The XXTEA key, packet format,
and RF parameters were extracted via SWD debug and SPI bus analysis.
