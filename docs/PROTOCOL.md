# Sunfree AC2001-15D 433MHz RF Protocol

Reverse-engineered from Tuya AC801B hub firmware (HDSC HC32L170 MCU).

## Encryption

- **Algorithm**: XXTEA (Corrected Block TEA)
- **Key**: `SUNFRE20201625` (ASCII, null-padded to 16 bytes)
  - Hex: `53 55 4e 46 52 45 32 30 32 30 31 36 32 35 00 00`
  - K[0..3] = `0x464e5553 0x30324552 0x36313032 0x00003532`
- **Block size**: Variable per command (3 or 4 x 32-bit words)
- **Rounds**: `52 / n + 1` where `n` = number of words
  - n=3: 18 rounds
  - n=4: 14 rounds

The key is hardcoded in flash at offset `0x8A60` — it's the product ID
string. This means **all Sunfree AC2001-15D motors/hubs share the same key**.

## Packet format (plaintext, before encryption)

```
B0   B1   B2   B3..BA   BB   BC   BD   BE   BF
LEN  SEQ  CMD  ADDRESS  FLG  00   DP   ACT  VAL
```

| Byte  | Name    | Description |
|-------|---------|-------------|
| B0    | LEN     | Payload length indicator (0x0B = 11 or 0x08 = 8) |
| B1    | SEQ     | Rolling sequence counter (increments each TX) |
| B2    | CMD     | Command class (0x01 = cover control) |
| B3-BA | ADDRESS | 8-byte motor device address (unique per motor) |
| BB    | FLAGS   | Flags (0x00 for normal commands) |
| BC    | -       | Always 0x00 for cover commands |
| BD    | DP      | Tuya DP (data point): 0x01 = cover control |
| BE    | ACTION  | See action codes table below |
| BF    | VALUE   | Action-dependent value |

### Action codes

| ACTION | Name | VALUE | XXTEA blocks | Description |
|--------|------|-------|--------------|-------------|
| 0x03 | STOP | 0xB7 (fixed) | n=3 (12 enc + 4 clear) | Stop motor |
| 0x04 | SET_POSITION | 0x00-0x64 | n=4 (16 enc) | Set position (0=open, 100=close) |
| 0x0F | GOTO_FAVOURITE | 0x03 | n=4 (16 enc) | Move to saved favourite position |
| 0x21 | SET_LIMIT | 0x01 | n=4 (16 enc) | Save current position as **open limit** |
| 0x21 | SET_LIMIT | 0x02 | n=4 (16 enc) | Save current position as **close limit** |
| 0x21 | SET_LIMIT | 0x03 | n=4 (16 enc) | Save current position as **favourite** |
| 0x22 | SET_DIRECTION | 0x00 | n=4 (16 enc) | Motor direction = **forward** |
| 0x22 | SET_DIRECTION | 0x01 | n=4 (16 enc) | Motor direction = **reverse** |
| 0x2A | STATUS_QUERY | 0x00 | n=4 (16 enc) | Poll motor for STATUS (battery+position) |
| 0x2C | CONFIG_QUERY | 0x00 | n=4 (16 enc) | Post-pairing config query (observed, untested) |
| 0x2D | DISCOVERY | n/a | n=3 (12 enc + 3 clear) | Pairing discovery (broadcast) |

## Encryption scope

- **Stop** commands (ACTION=0x03): Encrypt first 3 words (12 bytes, B0-BB).
  Bytes BC-BF transmitted in cleartext after the encrypted block.
- **Set position** commands (ACTION=0x04): Encrypt all 4 words (16 bytes).

## Rolling code

The sequence counter (B1) is a simple byte-wide incrementing counter,
NOT a cryptographic rolling code. The XXTEA encryption provides the
actual security — since the counter is inside the encrypted block,
replaying a captured packet without the key is not possible (except
for exact replays, which the motor may or may not reject).

## Example captures

### Stop command (SEQ=0x04)
```
Plaintext:  0b 04 01 15 24 13 42 04 25 0e 5b 00 | 00 01 03 b7
Encrypted:  63 3d c2 9a 56 7c 6d 1a c6 d0 4a 6a | 00 01 03 b7
                      encrypted (3 words)          | cleartext
```

### Open command (SEQ=0x02)
```
Plaintext:  0b 02 01 15 24 13 42 04 25 0e 5b 00 00 01 04 00
Encrypted:  b2 3a e1 b1 3e 65 45 44 82 b4 d0 fa 82 c0 54 88
                      all 4 words encrypted
```

### Close command (SEQ=0x03)
```
Plaintext:  0b 03 01 15 24 13 42 04 25 0e 5b 00 00 01 04 64
Encrypted:  a5 21 9b cd 28 66 76 04 80 59 00 35 ba b0 0f b7
                      all 4 words encrypted
```

## Motor → Hub response format

Motors respond after receiving commands. Responses use the same XXTEA
encryption with the same key.

### Response packet layout

Two types: **ACK** (short, n=3) and **status report** (full, n=6).

#### ACK packet (B0=0x08, n=3, 12 bytes encrypted + 4 clear)

```
B0   B1   B2   B3..BA              BB   | BC   BD   BE   BF
08   SEQ  01   ADDRESS(swapped)    FLG  | 00   VAL_HI  VAL_LO  STATUS
                encrypted 12 bytes       |    cleartext
```

- BB flags: 0x42 = command ACK, 0x41 = stop ACK

#### Status report (B0=0x0B, n=6, 24 bytes encrypted)

```
B0  B1   B2  B3..BA             BB  BC  BD  BE  BF  E0  E1   E2   E3     E4  E5  E6       E7
0B  SEQ  05  ADDRESS(swapped)   00  00  02  01  ST  PS  RPT  PA   RSSI   ??  PF  CUR_POS  TIME
                              all 24 bytes encrypted
```

| Byte | Name | Description |
|------|------|-------------|
| B15 (ST) | State | 0=stopped, 1=closing, 2=opening |
| B16 (PS) | Position (stopped) | Position 0-100 (% closed) when state=stopped |
| B17 (RPT) | Report type | 0xFF=regular status, 0x00=position+battery, 0x01/0x02=control ACK |
| B18 (PA) | Position (alt) | Position in battery report (when RPT=0x00) |
| B19 (BAT) | Battery % | **Battery state of charge** 0-100 → DP 106 |
| B20 | Unknown | Always 0x83 |
| B21 (PF) | Position flag | 0=position in E6 is valid |
| B22 (CUR_POS) | Current position | **Primary position field**: 0-100 (% closed) |
| B23 (TIME) | Time factor | Duration: value × 1000 = milliseconds |

**Address byte order is swapped** in responses: hub address first, then motor address
(vs. TX which has motor address first).

### Position

- **Primary field**: Byte 22 (CUR_POS), range 0-100 representing % closed
- **HA conversion**: `current_position = 100 - CUR_POS`
- **Verified**: CUR_POS=12 → HA position=88 ✓
- When stopped, byte 16 also carries the position value

### Battery

- **Primary field**: **Byte 19** (E3) — present in every status report → DP 106
- Range 0-100 representing battery state of charge percentage
- **Confirmed**: Tuya app shows 50%, last captured packet has byte 19 = 0x32 = 50
- Reported to Tuya cloud as DP 106 (vendor-specific), not the standard DP 13
- The firmware also has DP 13 battery support (from byte 16 when byte 17 == 0x00)
  but this alternate report type was never triggered during testing

### Signal strength

- RSSI is read from the radio chip register (not from packet data)
- Mapped to bars → DP 107:
  - ≥ 78: 4 bars
  - ≥ 58: 3 bars
  - ≥ 43: 2 bars
  - ≥ 33: 1 bar

### Example response captures

```
# Opening (state=2):
0b 55 05 04 25 0e 5b 15 24 13 42 00 00 02 01 02 07 ff 14 3c 83 00 00 07

# Stopped at 0% closed (fully open):
0b 56 05 04 25 0e 5b 15 24 13 42 00 00 02 01 00 00 ff 00 3c 83 00 00 00

# Closing (state=1, target=100%):
0b 57 05 04 25 0e 5b 15 24 13 42 00 00 02 01 01 00 ff 14 3c 83 00 64 61

# Stopped at 12% closed (HA pos=88):
0b 58 05 04 25 0e 5b 15 24 13 42 00 00 02 01 00 0c ff 00 32 83 00 0c 00
```

## Addressing

The 8-byte address in every packet is composed of two 4-byte parts:

```
TX (hub → motor):  HUB_ID(4)   + MOTOR_ID(4)
RX (motor → hub):  MOTOR_ID(4) + HUB_ID(4)     (halves swapped)
```

- **Hub ID**: Fixed per hub, same for all motors. Example: `15 24 13 42`
- **Motor ID**: Unique per motor, assigned during pairing.

### Confirmed motor addresses (from SWD capture)

| HA Motor | Motor ID | Device Table Index |
|----------|----------|-------------------|
| motor 15 | `04 4a 10 5b` | [15] |
| motor 21 | `05 25 0e 5b` | [19] |
| motor 22 | `04 25 0e 5b` | [18] |

### Dual-roller architecture

Each Sunfree AC2001-15D is a **dual-roller blind**: one physical motor
body contains a **day blind** and a **night blind**, each independently
controllable. One pairing registers **two motor IDs** (consecutive
entries, differing by 1 in the first byte):

- **Lower first byte** = Night blind (roller A)
- **Higher first byte** = Day blind (roller B)

In HA, each physical motor appears as **two cover entities**.

### Full device table (20 entries = 10 physical motors)

The hub stores up to 30 motor IDs in SRAM at `0x2000080C`.

```
Hub ID: 15 24 13 42

    Night (roller A)     Day (roller B)        Physical unit
 [ 0] 14 47 1d 42   [ 1] 15 47 1d 42         unit 0
 [ 2] 14 49 3b 42   [ 3] 15 49 3b 42         unit 1
 [ 4] 04 15 3a 5b   [ 5] 05 15 3a 5b         unit 2
 [ 6] 14 46 2c 42   [ 7] 15 46 2c 42         unit 3
 [ 8] 0a 2e 42 65   [ 9] 09 2e 42 65         unit 4
[10] 0a 34 2b 5b   [11] 0b 34 2b 5b         unit 5
[12] 0a 1e 19 5b   [13] 0b 1e 19 5b         unit 6
[14] 03 4a 10 5b   [15] 04 4a 10 5b         unit 7  (motor 15=day)
[16] 09 30 23 65   [17] 0a 30 23 65         unit 8
[18] 04 25 0e 5b   [19] 05 25 0e 5b         unit 9  (motor 22=night, 21=day)
```

### Multi-hub / multi-motor support

To control existing Tuya-paired motors from ESP32+CC1101:
1. Use the **same hub ID** as the Tuya hub (`15 24 13 42`)
2. Use each motor's **4-byte motor ID** from the device table
3. Motors accept commands matching their hub_id + motor_id

### Pairing new motors (ESP32 as hub)

The motor enters **LISTEN mode** after pressing M (it does NOT broadcast).
The hub must actively send discovery packets for the motor to hear.

#### Discovery packet (hub → motor)

A stop-style command (n=3 XXTEA, 12 bytes encrypted + 3 bytes cleartext)
addressed to broadcast (motor = `00000000`) with **ACTION = 0x2D**:

```
Plaintext: 0B seq 01 hub_id[4] 00000000 00 | 00 01 2D
Encrypted: XXTEA(first 12 bytes, n=3)     | cleartext tail
```

The hub sends this repeatedly (every ~1.5s) with full 160ms WOR preamble
during the scan window. The motor's listen window after pressing M is ~10s.

#### Motor pairing response (motor → hub)

A 20-byte packet (type 0x05 in D492 framing): first 16 bytes XXTEA n=4
encrypted, last 4 bytes cleartext. Contains **both** motor IDs:

```
Decrypted: 0B 00 02 night_id[4] hub_id[4] 00 00 01 2F day_first | day_last3[3] tail
```

| Bytes | Content |
|-------|---------|
| 0 | 0x0B (header) |
| 1 | 0x00 (seq) |
| 2 | 0x02 (pairing response CMD class) |
| 3-6 | Night motor ID (roller A) |
| 7-10 | Hub ID (echoed back) |
| 15 | First byte of day motor ID |
| 16-18 | Last 3 bytes of day motor ID (cleartext) |

#### Pairing flow

1. ESP32 generates a unique 4-byte hub ID (stored in NVS on first boot)
2. User presses "Start Pairing Scan" in HA → ESP32 begins sending
   ACTION=0x2D discovery packets with full WOR preamble every 1.5s
3. User presses M button on motor → motor enters listen mode (~10s)
4. Motor hears the discovery, stores the ESP32's hub ID
5. Motor responds with 20-byte pairing response containing both motor IDs
6. ESP32 receives response, extracts night + day motor IDs
7. User adds the motor IDs to their ESPHome YAML configuration

Maximum 30 motor IDs per hub (firmware limit `0x1E` = 15 physical units).

#### Post-pairing commands (observed from Tuya hub)

After the initial discovery+response, the Tuya hub sends additional
commands with actions 0x2A, 0x2C addressed to each motor ID. These
trigger the motor to respond with ACKs and STATUS reports. Action 0x2A
is used as a **status poll** — sending it at any time elicits a STATUS
response containing battery + position. The ESP32 exposes this as the
`request_status` HA service.

#### Hub → Motor ACK

After receiving a motor STATUS report (type 0x06), the hub sends an
ACK (type 0x03, n=3 XXTEA, 12 bytes) back to the motor. This mirrors
the motor's ACK format:

```
B0   B1   B2   B3..B6   B7..BA   BB
08   SEQ  01   HUB_ID   MOTOR_ID FLAGS
            encrypted 12 bytes
```

The motor may require this ACK to send further STATUS reports.
Without it, the pairing handshake shows the motor stops after one report.

## RF layer

Extracted from CMT2300A register banks in firmware (config 0x20 at flash 0x8880).

| Parameter | Value |
|-----------|-------|
| Carrier frequency | **433.920 MHz** (TX), 434.203 MHz (RX IF offset) |
| Modulation | **GFSK** |
| Symbol rate | **40 kbps** (CDR_BR_TH=650, Fxtal/650=40000) |
| Frequency deviation | **~38 kHz** (measured via SDR) |
| TX preamble (hub) | 0xAA, **800 bytes** (160ms, to span motor WOR cycle) |
| TX preamble (follow-up) | 0xAA, **8 bytes** (motor already awake) |
| RX preamble detect | 1 byte minimum |
| Sync word | **0x53524A44** ("SRJD", 4 bytes, MSB first on-air) |
| Packet mode | **Variable length** (length byte after sync word) |
| CRC | **CRC-8/MAXIM** (reflected poly 0x8C, init 0) appended after payload |
| FEC | Disabled |
| Data whitening | Disabled |
| Manchester | Disabled |
| Bit order | MSB first |
| Radio chip | CMT2300A (hub and motor) |
| Compatible TX/RX | CC1101 (ESP32 via ESPHome) |

### Over-the-air packet format

```
[Preamble AA×N] [Sync 53 52 4A 44] [LEN] [Payload] [CRC-8]
```

LEN = number of bytes following (payload + CRC byte):
- Set position: LEN=17 (16 encrypted + 1 CRC)
- Stop: LEN=16 (12 encrypted + 3 cleartext + 1 CRC)
- ACK: LEN=12 (TBC)
- Status: LEN=24 (TBC)

### CRC-8/MAXIM checksum

Every command packet ends with a CRC-8 byte computed over the payload bytes
(everything between LEN and CRC). Algorithm: CRC-8/MAXIM (Dallas 1-Wire),
reflected polynomial 0x8C (normal form 0x31), initial value 0x00.

Verified against FIFO captures from hub SPI bus:
- SET_POSITION SEQ=7: CRC over 16 encrypted → 0xFB ✓
- SET_POSITION SEQ=8: CRC over 16 encrypted → 0x2C ✓
- STOP SEQ=9: CRC over 15 bytes (12 enc + 3 clear) → 0xF9 ✓

### CC1101 6-bit sync alignment

The CMT2300A transmits a 4-byte sync `53 52 4A 44` ("SRJD"). The CC1101
only supports 16-bit hardware sync detection. The CC1101 sync is set to
`0xD492`, which is a **6-bit-shifted partial match** found within the
transition from preamble into the SRJD sync word:

```
Preamble: ...10101010 10101010
Sync:     01010011  01010010  01001010  01000100
                ↓↓                              
          bit 22: 11010100 10010010 = D4 92
```

D4 92 starts at bit 22 (6 bits into the first sync byte 0x53). This causes
the CC1101 to start capturing payload data **6 bits offset** from the
CMT2300A's byte alignment.

### CC1101 frame structure (20 bytes, after D4 92 sync)

After D4 92 detection, the CC1101 FIFO contains 20 bytes with this layout:

```
CC1101    Original CMT2300A data (6-bit shifted)
byte 0    [4A bit6-7] [44 bit0-5]          → always 0x91
byte 1    [44 bit6-7] [LEN bit0-5]         → encodes LEN:
                                               0x03 when LEN=0x0C (12, ACK)
                                               0x04 when LEN=0x10 (16, command)
                                               0x05 when LEN=0x14 (20, beacon)
                                               0x06 when LEN=0x18 (24, status)
byte 2    [LEN bit6-7] [payload0 bit0-5]   → first bits of encrypted payload
bytes 3-19  ...continuing 6-bit-shifted payload...
```

**The "0x91" header and "type" byte seen by the CC1101 are artifacts of
the bit-shift, not actual protocol fields.** Byte 0 is always 0x91 (residual
sync bits). Byte 1 encodes the CMT2300A LEN byte shifted by 2 bits.

### Extracting the real payload

To recover the original CMT2300A payload from a 20-byte CC1101 capture:

1. Convert the 20 bytes to a 160-bit stream
2. Skip the first 18 bits (2 residual sync + 8 bits of 0x44 + 8 bits of LEN)
3. Extract 8-bit bytes from bit 18 onwards
4. This yields up to 17 usable payload bytes

For **commands** (LEN=16): all 16 bytes are available (17 > 16) ✓
For **ACKs** (LEN=12): all 12 bytes are available ✓
For **status reports** (LEN=24): only 17 of 24 bytes available ✗

To capture full status reports, set `packet_length: 29` (18 prefix bits +
24×8 payload bits = 210 bits → ceil(210/8) = 27, rounded up with margin).

### CC1101 configuration

```yaml
cc1101:
  frequency: 433.95MHz
  modulation_type: GFSK
  symbol_rate: 40000
  fsk_deviation: 38kHz
  filter_bandwidth: 203kHz
  packet_mode: true
  packet_length: 20          # 29 for full status reports
  sync_mode: "16/16"
  sync1: 0xD4
  sync0: 0x92
  num_preamble: 6
```

### TX: Constructing CC1101 packets

To transmit a command via CC1101:

1. Build the 16-byte plaintext (see packet format above)
2. Encrypt with XXTEA (n=3/18 rounds for stop, n=4/14 rounds for position)
3. Prepend 18-bit header: `10` + `01000100` + LEN byte (0x10 for commands)
4. Concatenate payload bits after the header
5. Pack into 20 bytes (pad trailing bits with zeros)
6. The first byte will be 0x91 and second will be 0x04 — this is correct

The CC1101 then prepends preamble + D4 92 sync automatically.
The motor's CMT2300A will detect its own sync within the bitstream.

## Retransmission

Commands are retransmitted 2-3 times with identical frames. The motor
deduplicates based on the SEQ byte inside the encrypted payload.

**Note**: Earlier analysis suggested byte 19 in the D492 RX frame was an
incrementing retransmission counter. This was incorrect — that byte position
corresponds to CRC-8 and noise/padding bits from the 6-bit shift. The
apparent incrementing was an artifact of bit-shifted noise.

When transmitting from ESP32, send each command 2-3 times with ~20ms delay.
The frame content (including CRC) must be identical across retransmissions.

## Tuya DP mapping

| DP | Name | Type | Description |
|----|------|------|-------------|
| 1 | control | enum | 0=open, 1=stop, 2=close |
| 2 | percent_control | value | Position 0-100 (% open, HA convention) |
| 3 | percent_state | value | Current position 0-100 (% open) |
| 7 | work_state | enum | 0=closing, 1=opening |
| 10 | time_total | value | Operation duration in ms |
| 13 | battery_percentage | value | Battery 0-100% (only in RPT=0 reports) |
| 106 | battery_percentage | value | Battery 0-100% (vendor-specific DP) |
| 107 | signal_quality | value | Signal bars 1-4 (from radio RSSI) |

## Motor wake model (WOR)

The motor uses CMT2300A **Wake-on-Radio** (duty-cycle RX) to save battery.
The motor's radio is mostly asleep and wakes periodically to check for
incoming preamble. Confirmed via SPI register analysis of the hub's
CMT2300A TX configuration.

### Hub TX preamble (from SPI trace)

The hub configures the CMT2300A with an **800-byte preamble** for command
packets. This spans the motor's entire WOR sleep cycle, guaranteeing
preamble detection:

```
CUS_PKT2 (0x39) = 0x20   TX_PREAM_Size[7:0] = 32
CUS_PKT3 (0x3A) = 0x03   TX_PREAM_Size[15:8] = 3
→ TX_PREAM_Size = (3 << 8) | 32 = 800 units
With PREAM_LENG_UNIT = 0 (8 bits/unit): 800 bytes = 6400 bits
At 40 kbps: 160ms of continuous 0xAA preamble
```

For follow-up packets (motor already awake): `reg39=0x08, reg3A=0x00` →
8 bytes preamble (1.6ms). Sufficient because the motor stays in RX
briefly after receiving the first packet.

### Motor WOR parameters (inferred)

The motor's CMT2300A duty-cycle configuration is controlled by its MCU:

| Parameter | Estimated value |
|-----------|-----------------|
| WOR sleep period | ~100-150ms |
| RX detection window | ~1-2ms (preamble detect) |
| Post-RX listen time | ~100-500ms (for full packet) |
| Deep sleep threshold | Minutes of hub inactivity |

### Hub TX timing (from UART command log)

The hub retransmits commands every ~3 seconds via UART cmd(10):

```
23:32:36  cmd(10) → TX command (800-byte preamble + 16-byte payload)
23:32:39  cmd(10) → retransmit (3s, motor may be in deep sleep)
23:32:42  cmd(13) ← motor responded (ACK/status)
23:32:43  cmd(10) → follow-up (8-byte preamble, motor is awake)
23:32:45  cmd(22) ← done
```

Within each cmd(10), the RF MCU runs 6-8 TX/RX cycles (~500ms),
alternating between command packets (16 bytes) and shorter packets
(14 bytes, possibly status queries).

### ESP32+CC1101 TX strategy

The CC1101 auto-preamble is limited to 24 bytes (4.8ms). To match the
hub's 160ms preamble, we send multiple rapid preamble-only packets:

1. Set CC1101 sync to 0xAAAA (looks like preamble on-air)
2. Send 12 × 61-byte packets filled with 0xAA
   - Each: 24 (auto-preamble) + 2 (sync) + 61 (data) = 87 bytes = 17.4ms
   - Total: ~209ms of near-continuous preamble
3. Switch sync to 0x5352, send actual command with 3 retries

**Fallback (auto-piggyback)**: If the motor is in deep sleep (no WOR),
the command is also queued. When the motor eventually sends a periodic
status report (~10-30s), the ESP32 immediately fires the command during
the motor's guaranteed post-TX RX window.

### TX/RX cycling (status elicitation)

The motor only sends STATUS reports (type 0x06, 24-byte) after multiple
rounds of command→ACK exchanges.  The Tuya hub does **6-8 TX/RX cycles**
per command session, alternating between sending and listening:

```
T+0ms     HUB  → command (full WOR preamble)
T+~60ms   MOTOR → ACK
T+~400ms  HUB  → command (short preamble, motor awake)
T+~460ms  MOTOR → ACK
T+~800ms  HUB  → command
T+~860ms  MOTOR → ACK
T+~1100ms MOTOR → STATUS (battery + position!)
T+~1200ms HUB  → ACK
T+~1400ms MOTOR → STATUS (second report)
```

**Without** the follow-up retransmissions (TX/RX cycling), the motor
only sends ACKs — it never sends STATUS.  The ESP32 implementation
schedules 4 follow-up retransmissions ~200ms apart via the main loop,
giving the CC1101 time to receive motor responses between each TX.

## Implementation notes

To send a command from ESP32+CC1101:
1. Construct the plaintext packet (16 bytes)
2. Determine n_words based on command type (3 for stop, 4 for set_position)
3. Encrypt first `n_words * 4` bytes using XXTEA with the key
4. Append any unencrypted tail bytes
5. Send ~200ms WOR preamble (bit-banged via async serial on GDO0)
6. Schedule 4 follow-up retransmissions ~200ms apart (non-blocking, via loop)
7. The motor responds with ACK after each retransmission, then STATUS

To receive a motor response:
1. Listen on 433.95 MHz for GFSK packets (CC1101 sync 0xD492)
2. Decrypt using XXTEA (n=3 for ACK, n=6 for status)
3. Byte 22 = current position (% closed); invert for HA: `100 - value`
4. Byte 19 = battery percentage (0-100, always present in status reports)
5. Byte 15 = motor state (0=stopped, 1=closing, 2=opening)
