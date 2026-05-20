#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome {
namespace sunfree_blinds {

static const uint32_t XXTEA_DELTA = 0x9e3779b9;

static const uint32_t SUNFREE_KEY[4] = {
    0x464e5553,  // "SUNF" LE
    0x30324552,  // "RE20" LE
    0x36313032,  // "2016" LE
    0x00003532,  // "25\0\0" LE
};

// CC1101 packet_length — must match YAML config (D492 sync, PKTLEN=29)
static const int CC1101_PKT_LEN = 29;

enum class SunfreeCmd : uint8_t {
  STOP = 0x03,
  SET_POSITION = 0x04,
  GOTO_FAVOURITE = 0x0F,
  SET_LIMIT = 0x21,
  SET_DIRECTION = 0x22,
  STATUS_QUERY = 0x2A,
  DISCOVERY = 0x2D,
};

static constexpr uint8_t LIMIT_OPEN = 0x01;
static constexpr uint8_t LIMIT_CLOSE = 0x02;
static constexpr uint8_t LIMIT_FAVOURITE = 0x03;
static constexpr uint8_t DIR_FORWARD = 0x00;
static constexpr uint8_t DIR_REVERSE = 0x01;
static constexpr uint8_t GOTO_FAV_VALUE = 0x03;

struct SunfreePacket {
  uint8_t seq;
  uint8_t hub_id[4];
  uint8_t motor_id[4];
  SunfreeCmd action;
  uint8_t value;
  bool valid{false};
};

struct SunfreeResponse {
  uint8_t seq;
  uint8_t motor_id[4];
  uint8_t hub_id[4];
  uint8_t flags;
  bool is_ack{false};
  bool is_status{false};
  uint8_t ack_flags;
  uint8_t state;
  uint8_t position;
  uint8_t battery;
  bool valid{false};
};

inline void xxtea_encrypt(uint32_t *v, int n, const uint32_t *key) {
  int rounds = 52 / n + 1;
  uint32_t total = 0;
  for (int i = 0; i < rounds; i++) {
    total += XXTEA_DELTA;
    uint32_t e = (total >> 2) & 3;
    for (int p = 0; p < n; p++) {
      uint32_t y = v[(p + 1) % n];
      uint32_t z = v[(p + n - 1) % n];
      uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^
                     ((total ^ y) + (key[(p & 3) ^ e] ^ z));
      v[p] += mx;
    }
  }
}

inline void xxtea_decrypt(uint32_t *v, int n, const uint32_t *key) {
  int rounds = 52 / n + 1;
  uint32_t total = static_cast<uint32_t>(rounds) * XXTEA_DELTA;
  for (int i = 0; i < rounds; i++) {
    uint32_t e = (total >> 2) & 3;
    for (int p = n - 1; p >= 0; p--) {
      uint32_t z = v[(p + n - 1) % n];
      uint32_t y = v[(p + 1) % n];
      uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^
                     ((total ^ y) + (key[(p & 3) ^ e] ^ z));
      v[p] -= mx;
    }
    total -= XXTEA_DELTA;
  }
}

// =========================================================================
// RX: D492 sync (6-bit-shifted extraction)
//
// CC1101 syncs on D4 92 which appears 6 bits into the CMT2300A's 4-byte
// sync "SRJD" (53 52 4A 44). After sync, the FIFO contains 6-bit-shifted
// data:
//   byte 0 = 0x91 (residual sync bits 4A>>6 | 44<<2)
//   byte 1 = type byte encoding the CMT2300A LEN field shifted by 2 bits
//   bytes 2+ = bit-shifted encrypted payload
//
// The 18-bit prefix (2 residual sync bits + 8 bits of 0x44 + 8 bits of LEN)
// must be stripped, then the remaining bits are the original CMT2300A payload.
// =========================================================================

inline int d492_get_len(const uint8_t *cc, int cc_len) {
  if (cc_len < 2 || cc[0] != 0x91) return 0;
  uint8_t type_byte = cc[1];
  switch (type_byte) {
    case 0x03: return 12;  // ACK
    case 0x04: return 16;  // command
    case 0x05: return 20;  // beacon
    case 0x06: return 24;  // status
    default: return 0;
  }
}

inline bool d492_extract_payload(const uint8_t *cc, int cc_len, uint8_t *out, int payload_len) {
  int total_bits = cc_len * 8;
  int skip_bits = 18;  // 2 residual sync + 8 (0x44) + 8 (LEN)
  int need_bits = skip_bits + payload_len * 8;
  if (total_bits < need_bits) return false;

  for (int i = 0; i < payload_len; i++) {
    int bit_pos = skip_bits + i * 8;
    int byte_idx = bit_pos / 8;
    int bit_off = bit_pos % 8;

    uint8_t val;
    if (bit_off == 0) {
      val = cc[byte_idx];
    } else {
      val = (cc[byte_idx] << bit_off) | (cc[byte_idx + 1] >> (8 - bit_off));
    }
    out[i] = val;
  }
  return true;
}

// =========================================================================
// TX: 5352 sync (clean framing)
//
// For TX we switch CC1101 sync to 0x5352 so the on-air frame becomes:
//   [preamble] [53 52] [4A 44 LEN encrypted_payload padding]
//
// The motor's CMT2300A detects its 4-byte sync "SRJD" (53524A44) from
// our 2-byte CC1101 sync + the first 2 FIFO bytes. Then it reads LEN
// bytes of encrypted data.
// =========================================================================

// CRC-8/MAXIM (Dallas 1-Wire): reflected poly 0x8C (normal 0x31), init=0.
// The hub appends this CRC after the encrypted payload in every command packet.
inline uint8_t crc8_maxim(const uint8_t *data, int len) {
  uint8_t crc = 0;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0x8C;
      else
        crc >>= 1;
    }
  }
  return crc;
}

// TX frame: CC1101 hardware sync transmits 0x5352, FIFO starts with remaining
// SRJD bytes 0x4A44.  On air the motor sees:
//   [preamble] [53 52] [4A 44] [LEN] [payload] [CRC-8]
// LEN = payload_len + 1 (counts payload bytes + CRC byte).
// Frame is sized exactly to the data — no zero-padding.
inline std::vector<uint8_t> build_tx_frame(const uint8_t *payload, int payload_len) {
  int frame_len = 2 + 1 + payload_len + 1;  // 4A 44 + LEN + payload + CRC
  std::vector<uint8_t> frame(frame_len, 0);
  frame[0] = 0x4A;
  frame[1] = 0x44;
  frame[2] = static_cast<uint8_t>(payload_len + 1);
  memcpy(frame.data() + 3, payload, payload_len);
  frame[3 + payload_len] = crc8_maxim(payload, payload_len);
  return frame;
}

inline std::vector<uint8_t> build_stop_packet(const uint8_t *hub_id, const uint8_t *motor_id, uint8_t seq, bool swap = false) {
  uint8_t plain[12];
  plain[0] = 0x0B;
  plain[1] = seq;
  plain[2] = 0x01;
  if (swap) {
    memcpy(plain + 3, motor_id, 4);
    memcpy(plain + 7, hub_id, 4);
  } else {
    memcpy(plain + 3, hub_id, 4);
    memcpy(plain + 7, motor_id, 4);
  }
  plain[11] = 0x00;

  uint32_t words[3];
  memcpy(words, plain, 12);
  xxtea_encrypt(words, 3, SUNFREE_KEY);

  uint8_t payload[15];
  memcpy(payload, words, 12);
  payload[12] = 0x00;
  payload[13] = 0x01;
  payload[14] = 0x03;

  return build_tx_frame(payload, 15);
}

// Hub→motor ACK: 12-byte n=3 XXTEA packet (on-air type 0x03).
// The Tuya hub sends this after receiving a motor STATUS report;
// the motor expects the ACK to continue sending additional reports.
inline std::vector<uint8_t> build_ack_packet(const uint8_t *hub_id, const uint8_t *motor_id,
                                              uint8_t seq, uint8_t flags = 0x00,
                                              bool swap = false) {
  uint8_t plain[12];
  plain[0] = 0x08;
  plain[1] = seq;
  plain[2] = 0x01;
  if (swap) {
    memcpy(plain + 3, motor_id, 4);
    memcpy(plain + 7, hub_id, 4);
  } else {
    memcpy(plain + 3, hub_id, 4);
    memcpy(plain + 7, motor_id, 4);
  }
  plain[11] = flags;

  uint32_t words[3];
  memcpy(words, plain, 12);
  xxtea_encrypt(words, 3, SUNFREE_KEY);

  uint8_t payload[12];
  memcpy(payload, words, 12);
  return build_tx_frame(payload, 12);
}

// Generic n=4 command: 16 bytes fully XXTEA-encrypted.
// Used for position, limits, direction, favourite, and other config commands.
inline std::vector<uint8_t> build_n4_command(const uint8_t *hub_id, const uint8_t *motor_id,
                                              uint8_t seq, uint8_t action, uint8_t value,
                                              bool swap = false) {
  uint8_t plain[16];
  plain[0] = 0x0B;
  plain[1] = seq;
  plain[2] = 0x01;
  if (swap) {
    memcpy(plain + 3, motor_id, 4);
    memcpy(plain + 7, hub_id, 4);
  } else {
    memcpy(plain + 3, hub_id, 4);
    memcpy(plain + 7, motor_id, 4);
  }
  plain[11] = 0x00;
  plain[12] = 0x00;
  plain[13] = 0x01;
  plain[14] = action;
  plain[15] = value;

  uint32_t words[4];
  memcpy(words, plain, 16);
  xxtea_encrypt(words, 4, SUNFREE_KEY);

  uint8_t payload[16];
  memcpy(payload, words, 16);
  return build_tx_frame(payload, 16);
}

inline std::vector<uint8_t> build_position_packet(const uint8_t *hub_id, const uint8_t *motor_id,
                                                   uint8_t seq, uint8_t position, bool swap = false) {
  uint8_t plain[16];
  plain[0] = 0x0B;
  plain[1] = seq;
  plain[2] = 0x01;
  if (swap) {
    memcpy(plain + 3, motor_id, 4);
    memcpy(plain + 7, hub_id, 4);
  } else {
    memcpy(plain + 3, hub_id, 4);
    memcpy(plain + 7, motor_id, 4);
  }
  plain[11] = 0x00;
  plain[12] = 0x00;
  plain[13] = 0x01;
  plain[14] = 0x04;
  plain[15] = position;

  uint32_t words[4];
  memcpy(words, plain, 16);
  xxtea_encrypt(words, 4, SUNFREE_KEY);

  uint8_t payload[16];
  memcpy(payload, words, 16);

  return build_tx_frame(payload, 16);
}

// =========================================================================
// RX parsers (operate on D492-extracted payloads)
// =========================================================================

inline bool parse_command(const uint8_t *cc, int cc_len, SunfreePacket &pkt) {
  int len = d492_get_len(cc, cc_len);
  if (len != 16) return false;

  uint8_t data[16];
  if (!d492_extract_payload(cc, cc_len, data, 16)) return false;

  // Try n=4 (position command) first
  uint32_t words[4];
  memcpy(words, data, 16);
  xxtea_decrypt(words, 4, SUNFREE_KEY);
  uint8_t dec[16];
  memcpy(dec, words, 16);

  if (dec[0] == 0x0B && dec[2] == 0x01 && dec[14] == 0x04) {
    pkt.seq = dec[1];
    memcpy(pkt.hub_id, dec + 3, 4);
    memcpy(pkt.motor_id, dec + 7, 4);
    pkt.action = SunfreeCmd::SET_POSITION;
    pkt.value = dec[15];
    pkt.valid = true;
    return true;
  }

  // Try n=3 (stop command): first 12 bytes encrypted, last 4 plaintext
  memcpy(words, data, 12);
  xxtea_decrypt(words, 3, SUNFREE_KEY);
  memcpy(dec, words, 12);

  if (dec[0] == 0x0B && dec[2] == 0x01 && data[14] == 0x03) {
    pkt.seq = dec[1];
    memcpy(pkt.hub_id, dec + 3, 4);
    memcpy(pkt.motor_id, dec + 7, 4);
    pkt.action = SunfreeCmd::STOP;
    pkt.value = 0;
    pkt.valid = true;
    return true;
  }

  return false;
}

inline bool parse_ack(const uint8_t *cc, int cc_len, SunfreeResponse &resp) {
  int len = d492_get_len(cc, cc_len);
  if (len != 12) return false;

  uint8_t data[12];
  if (!d492_extract_payload(cc, cc_len, data, 12)) return false;

  uint32_t words[3];
  memcpy(words, data, 12);
  xxtea_decrypt(words, 3, SUNFREE_KEY);
  uint8_t dec[12];
  memcpy(dec, words, 12);

  if (dec[0] != 0x08) return false;

  resp.seq = dec[1];
  resp.is_ack = true;
  memcpy(resp.motor_id, dec + 3, 4);
  memcpy(resp.hub_id, dec + 7, 4);
  resp.ack_flags = dec[11];
  resp.valid = true;
  return true;
}

inline bool parse_status(const uint8_t *cc, int cc_len, SunfreeResponse &resp) {
  int len = d492_get_len(cc, cc_len);
  if (len != 24) return false;

  uint8_t data[24];
  if (!d492_extract_payload(cc, cc_len, data, 24)) return false;

  uint32_t words[6];
  memcpy(words, data, 24);
  xxtea_decrypt(words, 6, SUNFREE_KEY);
  uint8_t dec[24];
  memcpy(dec, words, 24);

  if (dec[0] != 0x0B) return false;

  resp.seq = dec[1];
  resp.is_status = true;
  memcpy(resp.motor_id, dec + 3, 4);
  memcpy(resp.hub_id, dec + 7, 4);
  resp.state = dec[15];
  resp.position = dec[22];
  resp.battery = dec[19];
  resp.valid = true;
  return true;
}

// =========================================================================
// Pairing: discovery packet builder
//
// Captured from Tuya hub pairing session. The discovery packet is a
// stop-style command (n=3 XXTEA, 12 enc + 4 clear) addressed to
// broadcast (motor=00000000) with ACTION=0x2D:
//
//   Plaintext: 0B seq 01 hub_id[4] 00000000 00 | 00 01 2D value
//   Encrypted: XXTEA(first 12 bytes, n=3) | last 4 bytes cleartext
// =========================================================================

inline std::vector<uint8_t> build_discovery_packet(const uint8_t *hub_id, uint8_t seq) {
  uint8_t plain[12];
  plain[0] = 0x0B;
  plain[1] = seq;
  plain[2] = 0x01;  // CMD class 1 (same as normal commands)
  memcpy(plain + 3, hub_id, 4);
  memset(plain + 7, 0, 4);  // broadcast motor address
  plain[11] = 0x00;

  uint32_t words[3];
  memcpy(words, plain, 12);
  xxtea_encrypt(words, 3, SUNFREE_KEY);

  uint8_t payload[15];
  memcpy(payload, words, 12);
  payload[12] = 0x00;
  payload[13] = 0x01;
  payload[14] = 0x2D;  // discovery action code

  return build_tx_frame(payload, 15);
}

// Motor pairing response: 20 bytes, XXTEA n=4 (first 16 bytes) + 4 cleartext.
// Captured from real pairing session:
//   Decrypted: 0B 00 02 night_id[4] hub_id[4] 00 00 01 2F day_first_byte | day_last3[3] tail
// Contains both motor IDs (night + day) in a single response.
struct SunfreePairingResponse {
  uint8_t raw[20];
  uint8_t night_id[4];   // first motor ID (night/roller A)
  uint8_t day_id[4];     // second motor ID (day/roller B)
  uint8_t hub_id[4];     // hub ID the motor paired with
  bool valid{false};
};

inline bool parse_pairing_response(const uint8_t *cc, int cc_len, SunfreePairingResponse &resp) {
  int len = d492_get_len(cc, cc_len);
  if (len != 20) return false;

  uint8_t data[20];
  if (!d492_extract_payload(cc, cc_len, data, 20)) return false;
  memcpy(resp.raw, data, 20);

  // Decrypt first 16 bytes with XXTEA n=4, last 4 bytes are cleartext
  uint32_t words[4];
  memcpy(words, data, 16);
  xxtea_decrypt(words, 4, SUNFREE_KEY);
  uint8_t dec[20];
  memcpy(dec, words, 16);
  memcpy(dec + 16, data + 16, 4);  // cleartext tail

  if (dec[0] != 0x0B) return false;
  // dec[2] varies by motor revision: 0x02 (original capture), 0x05 (AC2001-15D)

  // Night motor ID at bytes 3-6
  memcpy(resp.night_id, dec + 3, 4);
  // Hub ID at bytes 7-10
  memcpy(resp.hub_id, dec + 7, 4);
  // Day motor ID: first byte at dec[15], last 3 bytes at dec[16-18]
  resp.day_id[0] = dec[15];
  memcpy(resp.day_id + 1, dec + 16, 3);

  resp.valid = true;
  return true;
}

inline std::string format_motor_id(const uint8_t *id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
  return std::string(buf);
}

inline bool parse_motor_id(const std::string &s, uint8_t *out) {
  if (s.size() != 8) return false;
  for (int i = 0; i < 4; i++) {
    char hex[3] = {s[i * 2], s[i * 2 + 1], 0};
    char *end;
    long val = strtol(hex, &end, 16);
    if (*end != 0) return false;
    out[i] = static_cast<uint8_t>(val);
  }
  return true;
}

}  // namespace sunfree_blinds
}  // namespace esphome
