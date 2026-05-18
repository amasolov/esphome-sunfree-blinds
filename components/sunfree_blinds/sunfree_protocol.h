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
};

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
