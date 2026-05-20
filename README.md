# ESPHome Sunfree Blinds

[![ESPHome](https://img.shields.io/badge/ESPHome-external_component-blue)](https://esphome.io)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**Local control for 433 MHz motorized roller blinds** -- replaces the
cloud-dependent Tuya AC801B hub with an ESP32 + CC1101 radio running ESPHome.

## Supported brands

These blinds all use **Sunfree motors** with the same encrypted 433 MHz
protocol. If yours came with a Tuya/Smart Life hub (model AC801B), this
component can replace it:

| Brand | Sold in | Notes |
|-------|---------|-------|
| **Persilux** | US, Amazon | Popular motorized roller shades |
| **Blinds Online** | AU, NZ | Custom-fit roller blinds |
| **Tuiss** / **Blinds2Go** | UK, EU | SmartView RF models |
| **Zemismart** (some models) | AliExpress, Amazon | Tuya-compatible 433 MHz variants |
| **Sunfree** OEM | Global | AC2001-15D motor, various white-labels |

> **Not sure if yours is compatible?** Check: does it use a small WiFi hub
> (AC801B) that connects to the Tuya/Smart Life app and controls blinds
> over 433 MHz RF? If yes, this component works.

## Features

- **Open / Close / Stop / Set Position** with percentage control
- **Battery level** reported as a Home Assistant sensor
- **Motor groups** -- control entire rooms with a single command
- **Bidirectional** -- receives position and battery status from motors
- **Motor configuration** -- set direction, limits, and favourite position
  from Home Assistant services
- **Dual-roller support** -- day and night rollers exposed as separate covers
- **Motor pairing** -- pair new motors directly from HA, no Tuya hub needed
- **Status polling** -- request battery/position updates on demand
- **Wake-on-Radio** -- 800-byte preamble wakes sleeping motors reliably
- **TX/RX cycling** -- follow-up retransmissions mimic the Tuya hub for
  reliable delivery to battery-powered motors
- **No cloud dependency** -- direct 433 MHz RF, no internet required
- **Coexistence** -- can run alongside a Tuya hub or replace it entirely

## Hardware

| Component | Description | Approx. cost |
|-----------|-------------|--------------|
| ESP32-S3 dev board | ESP32-S3-DevKitC-1 recommended | ~$10 |
| CC1101 module | 433 MHz variant with antenna | ~$3 |
| Dupont wires | 7 wires for SPI + GDO0 + power | ~$1 |

Total: **~$15** to replace the $30-50 Tuya hub.

> **PSRAM recommended** for setups with more than 8 covers. The ESP32-S3
> with 8 MB PSRAM (N8R8 or N16R8 module) provides ample memory for 20+
> covers. Add `psram:` to your YAML -- see the example below.

### Wiring (ESP32-S3)

| Signal   | GPIO   | CC1101 Pin |
|----------|--------|------------|
| SPI CLK  | GPIO12 | SCLK       |
| SPI MOSI | GPIO11 | MOSI       |
| SPI MISO | GPIO13 | MISO       |
| CS       | GPIO5  | CSn        |
| GDO0     | GPIO4  | GDO0       |
| 3.3 V    | 3V3    | VCC        |
| GND      | GND    | GND        |

See [docs/WIRING.md](docs/WIRING.md) for a detailed diagram and antenna
guidance.

## Quick start

### 1. Install the component

```yaml
external_components:
  - source: {type: git, url: https://github.com/amasolov/esphome-sunfree-blinds}
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
  fsk_deviation: 20kHz
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

### 3. Enable custom services

```yaml
api:
  encryption:
    key: !secret api_key
  custom_services: true
```

### 4. Configure the hub and covers

```yaml
sunfree_blinds:
  id: sunfree_hub
  cc1101_id: cc1101_radio
  # hub_id: "a2fb24d8"  # optional -- set to migrate from Tuya hub
  groups:
    - name: "Bedroom Night"
      motor_ids: ["15471d42", "15493b42"]
    - name: "Bedroom Day"
      motor_ids: ["14471d42", "14493b42"]

cover:
  - platform: sunfree_blinds
    name: "Bedroom Left Night"
    motor_id: "15471d42"
    battery:
      name: "Bedroom Left Night Battery"

  - platform: sunfree_blinds
    name: "Bedroom Left Day"
    motor_id: "14471d42"
    battery:
      name: "Bedroom Left Day Battery"
```

### 5. Pair your motors

Add pairing buttons and diagnostic sensors:

```yaml
button:
  - platform: template
    name: "Start Pairing Scan"
    on_press:
      then:
        - lambda: id(sunfree_hub).start_scan();

  - platform: template
    name: "Stop Pairing Scan"
    on_press:
      then:
        - lambda: id(sunfree_hub).stop_scan();

text_sensor:
  - platform: template
    name: "Hub ID"
    update_interval: 60s
    lambda: return id(sunfree_hub).get_hub_id_str();

  - platform: template
    name: "Pairing Status"
    update_interval: 5s
    lambda: return id(sunfree_hub).get_pairing_status();
```

Then:
1. Flash and boot the ESP32
2. Press **Start Pairing Scan** in Home Assistant
3. Press the **M button** on the motor (within 10 seconds)
4. The "Pairing Status" sensor shows: `PAIRED! night=XXXXXXXX day=YYYYYYYY`
5. Add the motor IDs to your YAML and reflash

See [example.yaml](example.yaml) for a complete working configuration.

## Motor groups

Groups let you control multiple motors with a single command -- useful for
rooms with several blinds. Define groups in the hub config:

```yaml
sunfree_blinds:
  id: sunfree_hub
  cc1101_id: cc1101_radio
  groups:
    - name: "Living Room Day"
      motor_ids: ["04250e5b", "092e4265", "09302365"]
    - name: "Living Room Night"
      motor_ids: ["05250e5b", "0a2e4265", "0a302365"]
```

Call group commands via the HA service:

```yaml
service: esphome.sunfree_blinds_group_command
data:
  group: "Living Room Day"
  action: "open"    # open, close, stop, or 0-100 for position
```

All motors in the group receive the command in a single WOR preamble
transmission with follow-up retransmissions for reliability.

## Motor configuration

Configure direction, limits, and favourite position via HA services -- no
YAML changes needed.

```yaml
service: esphome.sunfree_blinds_send_config
data:
  motor_id: "15471d42"
  action_type: "direction_reverse"
```

| `action_type` | Description |
|----------------|-------------|
| `direction_forward` | Set motor direction to forward |
| `direction_reverse` | Set motor direction to reverse |
| `set_open_limit` | Save current position as the open limit |
| `set_close_limit` | Save current position as the close limit |
| `save_favourite` | Save current position as the favourite |
| `goto_favourite` | Move to the saved favourite position |
| `request_status` | Poll a motor for battery and position |

Use `motor_id: "all"` with `action_type: "request_status"` to poll every
motor sequentially.

## Status polling

The component polls motors for battery and position using TX/RX cycling
that mimics the original Tuya hub:

- Each command is followed by 2-6 retransmissions with fresh sequence
  numbers, spaced 200 ms apart
- Motors respond with STATUS packets containing position and battery level
- The hub ACKs each STATUS to keep the motor reporting
- Sequential polling with 500 ms cooldown between motors prevents
  cross-talk

Battery and position update automatically after any open/close/stop
command. Use `request_status` to poll on demand.

## Hub ID

When `hub_id` is omitted, a random 4-byte ID is generated on first boot
and stored in NVS (Non-Volatile Storage):

- **Survives** reboots, OTA updates, and config changes
- **Lost on** full flash erase (`--erase-flash`) -- motors must be re-paired

To see your hub ID, add a text sensor (shown above). Once known, pin it
in the YAML as a backup: `hub_id: "a2fb24d8"`.

### Migrating from a Tuya AC801B hub

If your blinds are already paired to a Tuya hub, set `hub_id` to the
hub's ID so motors accept commands without re-pairing.

**Option A: Sniff RF traffic** (easiest)

Flash this component with any placeholder `hub_id`, enable DEBUG logging,
and trigger the Tuya hub to send a command. The ESP32 logs overheard
commands:

```
[sunfree] RX CMD h=15241342 m=04250e5b a=4 v=100 s=0x30
```

Here `h=15241342` is the hub ID and `m=04250e5b` is the motor ID.

**Option B: SWD debug dump** (requires hardware debugger)

Connect a debugger to the hub's MCU (HDSC HC32L170) and read SRAM at
`0x2000080C`. The hub ID is at the start, followed by motor ID pairs.

### Dual-roller motor IDs

Each Sunfree unit has **two** rollers with sequential motor IDs:

- **Night roller**: lower first byte (e.g. `04250e5b`)
- **Day roller**: higher first byte (e.g. `05250e5b`)

## PSRAM configuration

For ESP32-S3 boards with PSRAM (recommended for 8+ covers):

```yaml
esp32:
  board: esp32-s3-devkitc-1
  flash_size: 16MB
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_ESPTOOLPY_FLASHMODE_QIO: "y"

psram:
  mode: octal
  speed: 80MHz
```

> **Build cache warning**: If using the HA ESPHome dashboard, the build
> cache may not pick up `sdkconfig_options` changes. Compile locally with
> `esphome compile` after deleting the build directory to ensure PSRAM
> is enabled.

## Web UI (optional)

When `web_server` is enabled in your YAML, the component serves a control
page at `http://<device-ip>/sunfree` with motor status, buttons, and
configuration commands. The web UI is optional and automatically disabled
when `web_server` is not configured.

## How it works

The component implements the full Sunfree/Tuya 433 MHz protocol:

1. **XXTEA encryption** with key `SUNFRE20201625`
2. **GFSK modulation** at 40 kbps, 20 kHz deviation
3. **Split-frequency TX/RX** -- 433.950 MHz for commands, 433.933 MHz for
   WOR pairing
4. **CRC-8/MAXIM** on every packet
5. **800-byte WOR preamble** via async serial bit-banging on GDO0
6. **TX/RX cycling** with fresh sequence numbers to avoid motor dedup
7. **Hub-to-motor ACK** after STATUS reports

For full protocol details, see [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Troubleshooting

### Motor does not respond

- Verify `hub_id` and `motor_id` are correct (8 hex chars each)
- Check CC1101 wiring (SPI + GDO0) and that it's a **433 MHz** module
- Try moving the ESP32 + CC1101 closer to the motor
- Check logs for `TX` messages confirming commands are being sent

### Motor responds intermittently

- Ensure `fsk_deviation: 20kHz` (not 38 kHz -- this was a common early
  misconfiguration)
- The CC1101 crystal may have a frequency offset. Adjust the TX frequency
  in `sunfree_hub.h` if needed
- Add more follow-up retransmissions if motors are far away

### Battery percentage not updating

- Battery is reported in STATUS packets sent after commands
- Use `request_status` to poll a specific motor or all motors
- The motor must be paired and responding to commands

### Position values seem inverted

- Sunfree uses 0 = open, 100 = closed
- Home Assistant uses 0 = closed, 100 = open
- The component handles this conversion automatically
- Use `invert_position: true` on the cover if your motor direction is
  reversed

### API connection errors (`errno=128`)

- Check for stale `esphome logs` processes (`ps ax | grep esphome`)
- Kill any zombie log sessions -- each holds an API connection
- The device supports 3-4 concurrent API clients maximum

## Project structure

```
components/
  sunfree_blinds/
    __init__.py          Hub platform (YAML config -> C++ codegen)
    cover.py             Cover platform (per-motor config)
    sunfree_protocol.h   Encryption, packet framing, CRC-8, parsers
    sunfree_hub.h        Radio control, WOR preamble, TX/RX cycling
    sunfree_cover.h      Cover entity, position mapping, status handling
    sunfree_web.h        Optional web UI and JSON API
example.yaml             Complete working configuration
docs/
  PROTOCOL.md            Full RF protocol specification
  WIRING.md              Hardware wiring guide
```

## License

MIT License. See [LICENSE](LICENSE).

## Credits

Protocol reverse-engineered from the Tuya AC801B hub firmware
(HDSC HC32L170 MCU + CMT2300A radio). XXTEA key, packet format, and RF
parameters were extracted via SWD debug and SPI bus analysis.
RF parameters validated independently with RTL-SDR wideband capture.
