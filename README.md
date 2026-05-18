# esphome-sunfree-blinds

ESPHome custom component for **Sunfree AC2001-15D** 433 MHz motorized blinds.
Replaces the Tuya AC801B cloud hub with a fully local **ESP32 + CC1101** bridge.

## Features

- **Open / Close / Stop / Set Position** with precise percentage control
- **Motor configuration** -- set direction (forward/reverse), open/close limits,
  and favourite position directly from Home Assistant
- **Battery state of charge** reported as a Home Assistant sensor
- **Dual-roller support** -- each Sunfree unit has independent day and night
  rollers, exposed as separate cover entities
- **Motor pairing** -- pair new motors directly from HA, no Tuya hub needed
- **Wake-on-Radio (WOR)** -- 800-byte preamble wakes sleeping motors, matching
  the original hub's behavior
- **Built-in web UI** at `/sunfree` -- control and configure all motors from any browser
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
  - source: {type: git, url: https://github.com/amasolov/esphome-sunfree-blinds}
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

### 3. Enable custom services

The component registers an HA service for motor configuration, which
requires `custom_services: true` in the `api:` section:

```yaml
api:
  encryption:
    key: !secret api_key
  custom_services: true
```

### 4. Configure the hub

```yaml
sunfree_blinds:
  id: sunfree_hub
  # hub_id is optional — a random 4-byte ID is generated on first boot
  # and stored in NVS (Non-Volatile Storage). It persists across reboots
  # and OTA reflashes. Set it explicitly only when migrating from a Tuya
  # AC801B hub, or to pin it for disaster recovery.
  # hub_id: "15241342"
  cc1101_id: cc1101_radio
```

#### Hub ID persistence

When `hub_id` is omitted, the component generates a random 4-byte ID on
first boot and saves it to NVS. This ID:

- **Survives reboots and OTA updates** -- NVS is a separate flash
  partition from the application firmware
- **Survives config changes** -- the NVS key is based on a fixed string,
  not the device name or YAML content
- **Is lost on full flash erase** (`esphome run` with `--erase-flash`,
  or `idf.py erase-flash`) -- a new ID will be generated and all motors
  must be re-paired

To see your hub ID, add this text sensor:

```yaml
text_sensor:
  - platform: template
    name: "Hub ID"
    update_interval: 60s
    lambda: return id(sunfree_hub).get_hub_id_str();
```

Once you know your hub ID, you can optionally pin it in the YAML as a
backup: `hub_id: "a2fb24d8"`. This way, even a full flash erase won't
require re-pairing.

### 5. Pair your motors

Add the pairing and diagnostic entities to your YAML:

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
2. Check the **Hub ID** sensor to confirm your hub identity
3. Press **Start Pairing Scan** in Home Assistant
4. Press the **M button** on the motor (within 10 seconds)
5. The "Pairing Status" sensor shows: `PAIRED! night=XXXXXXXX day=YYYYYYYY`
6. Add the motor IDs to your YAML:

```yaml
cover:
  - platform: sunfree_blinds
    name: "Office Night Blind"
    motor_id: "XXXXXXXX"    # night roller motor ID from pairing
    battery:
      name: "Office Night Battery"

  - platform: sunfree_blinds
    name: "Office Day Blind"
    motor_id: "YYYYYYYY"    # day roller motor ID from pairing
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

### Pairing new motors

Pairing is fully supported directly from the ESP32 — no Tuya hub needed.

1. Press **Start Pairing Scan** in HA (the ESP32 begins transmitting
   discovery packets every 1.5 seconds)
2. Press the **M button** on the motor — it enters listen mode for ~10s
3. The motor hears the ESP32's discovery and responds with both motor IDs
4. The **Pairing Status** sensor shows: `PAIRED! night=XXXXXXXX day=YYYYYYYY`
5. Add the IDs to your YAML and reflash

See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full protocol details.

## Motor configuration

After pairing, you can configure each motor's direction, travel limits,
and favourite position via an **HA service** -- no extra YAML needed.
The hub automatically registers `esphome.<device>_send_config`.

### Usage (HA Developer Tools > Services)

```yaml
service: esphome.sunfree_blinds_send_config
data:
  motor_id: "034a105b"
  action_type: "direction_forward"
```

### Workflow

1. Move the blind to the desired position using the cover controls
2. Call the service with the appropriate `action_type` to save that
   position as a limit or favourite
3. For direction changes, just call the service -- no positioning needed

### Available `action_type` values

| action_type | Description |
|-------------|-------------|
| `direction_forward` | Set motor direction to forward |
| `direction_reverse` | Set motor direction to reverse |
| `set_open_limit` | Save current position as the open limit |
| `set_close_limit` | Save current position as the close limit |
| `save_favourite` | Save current position as the favourite |
| `goto_favourite` | Move to the saved favourite position |

## Web UI

The component serves a built-in control page at `http://<device-ip>/sunfree`.
No extra configuration needed -- it hooks into the existing ESPHome web server.

Features:
- Live motor list with position and battery status (auto-refreshes)
- Open / Close / Stop buttons and position slider per motor
- All configuration commands: direction, limits, favourite
- Pairing scan start/stop

The JSON API behind the UI is also available for scripting:
- `GET /sunfree/status` -- hub and motor state as JSON
- `POST /sunfree/cmd?motor=XXXX&action=open` -- send a command

## Configuration reference

### Hub (`sunfree_blinds`)

| Key | Required | Description |
|-----|----------|-------------|
| `id` | Yes | ESPHome component ID |
| `hub_id` | No | 4-byte hub identifier (8 hex characters). If omitted, a random ID is generated on first boot and persisted in NVS. |
| `cc1101_id` | Yes | ID of the CC1101 component |

**Runtime accessors** (for use in template sensors/lambdas):

| Method | Returns | Description |
|--------|---------|-------------|
| `get_hub_id_str()` | `std::string` | Current hub ID as 8-char hex string |
| `get_pairing_status()` | `std::string` | Pairing scan status (idle / SCANNING / PAIRED) |
| `start_scan()` | `void` | Begin pairing discovery (120s timeout) |
| `stop_scan()` | `void` | Cancel active pairing scan |

### Cover (`platform: sunfree_blinds`)

| Key | Required | Description |
|-----|----------|-------------|
| `name` | Yes | Entity name in Home Assistant |
| `motor_id` | Yes | 4-byte motor identifier (8 hex characters) |
| `battery` | No | Sub-schema for battery sensor (sensor config) |

### Service (`send_config`)

Automatically registered -- no YAML config needed. Call via
`esphome.<device>_send_config` with two string parameters:

| Parameter | Description |
|-----------|-------------|
| `motor_id` | 4-byte motor identifier (8 hex characters) |
| `action_type` | One of: `direction_forward`, `direction_reverse`, `set_open_limit`, `set_close_limit`, `save_favourite`, `goto_favourite` |

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
    sunfree_hub.h        CC1101 radio control, WOR preamble, config service
    sunfree_cover.h      Cover entity, position mapping, status handling
    sunfree_web.h        Built-in web UI and JSON API handlers
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
