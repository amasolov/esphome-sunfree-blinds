#pragma once
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/cc1101/cc1101.h"
#include "sunfree_protocol.h"
#include <map>
#include <functional>
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_timer.h"

namespace esphome {
namespace sunfree_blinds {

static const char *const TAG = "sunfree";

// action codes for pending commands
static constexpr uint8_t ACTION_NONE = 0xFF;
static constexpr uint8_t ACTION_STOP = 0;
static constexpr uint8_t ACTION_OPEN = 1;
static constexpr uint8_t ACTION_CLOSE = 2;
static constexpr uint8_t ACTION_POSITION = 3;

class SunfreeCover;

class SunfreeHub : public Component {
 public:
  void set_cc1101(cc1101::CC1101Component *radio) { this->radio_ = radio; }
  void set_hub_id(const std::string &id) { parse_motor_id(id, this->hub_id_); }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void setup() override {
    ESP_LOGI(TAG, "Hub ID: %s", format_motor_id(this->hub_id_).c_str());
    ESP_LOGI(TAG, "Registered %d cover(s)", this->covers_.size());
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    ESP_LOGI(TAG, "CC1101: CRC and whitening disabled for Sunfree protocol");
  }

  void register_cover(SunfreeCover *cover);

  void set_swap_fields(bool swap) { this->swap_fields_ = swap; }
  bool get_swap_fields() const { return this->swap_fields_; }

  // Primary TX: long preamble to wake motor from WOR, then command
  void send_command(const uint8_t *motor_id, uint8_t action, uint8_t position = 0) {
    uint8_t seq = this->next_seq();
    std::vector<uint8_t> pkt = this->build_command_packet_(motor_id, action, position, seq);
    const char *act_name = action_name_(action);
    ESP_LOGI(TAG, "TX %s seq=0x%02x motor=%s", act_name, seq,
             format_motor_id(motor_id).c_str());
    this->transmit_with_preamble_(pkt);

    // Also arm auto-piggyback as fallback for deep-sleeping motors
    this->arm_piggyback_(motor_id, action, position);
  }

  // Quick TX for piggyback: motor is already awake, skip preamble
  void send_quick(const uint8_t *motor_id, uint8_t action, uint8_t position = 0) {
    uint8_t seq = this->next_seq();
    std::vector<uint8_t> pkt = this->build_command_packet_(motor_id, action, position, seq);
    const char *act_name = action_name_(action);
    ESP_LOGI(TAG, "QUICK TX %s seq=0x%02x motor=%s", act_name, seq,
             format_motor_id(motor_id).c_str());
    this->transmit_command_only_(pkt, 3);
  }

  // Replay: retransmit a captured hub CMD packet on the TX path
  // This tests whether our RF parameters can deliver a known-good packet.
  // The captured D492 RX frame must be bit-shifted back to CMT2300A framing.
  void replay_captured() {
    if (!this->have_capture_) {
      ESP_LOGW(TAG, "Replay: no capture available");
      this->piggyback_status_ = "REPLAY: no capture";
      return;
    }
    // Extract the CMT2300A payload (16 bytes) from the D492 RX frame
    uint8_t payload[16];
    if (!d492_extract_payload(this->captured_raw_.data(), this->captured_raw_.size(), payload, 16)) {
      ESP_LOGW(TAG, "Replay: extraction failed");
      this->piggyback_status_ = "REPLAY: extract fail";
      return;
    }
    // Build a TX frame from the extracted payload (includes CRC-8)
    std::vector<uint8_t> frame = build_tx_frame(payload, 16);
    char hex[30 * 3 + 1];
    for (int i = 0; i < static_cast<int>(frame.size()); i++) snprintf(hex + i * 3, 4, "%02x ", frame[i]);
    ESP_LOGI(TAG, "REPLAY TX frame: %s", hex);
    this->piggyback_status_ = "REPLAYING...";
    this->transmit_with_preamble_(frame);
    this->piggyback_status_ = "REPLAYED";
  }

  uint32_t get_rx_packet_count() const { return this->rx_packet_count_; }
  uint32_t get_rx_valid_count() const { return this->rx_valid_count_; }
  uint32_t get_rx_status_count() const { return this->rx_status_count_; }
  uint32_t get_rx_ack_count() const { return this->rx_ack_count_; }
  uint32_t get_rx_cmd_count() const { return this->rx_cmd_count_; }
  const std::string &get_last_rx_motor() const { return this->last_rx_motor_; }
  const std::string &get_last_rx_info() const { return this->last_rx_info_; }
  const std::string &get_last_cmd_info() const { return this->last_cmd_info_; }
  const std::string &get_piggyback_status() const { return this->piggyback_status_; }

  void on_cc1101_packet(const std::vector<uint8_t> &data, float rssi);

 protected:
  cc1101::CC1101Component *radio_{nullptr};
  uint8_t hub_id_[4]{};
  uint8_t seq_{0x80};
  uint8_t overheard_seq_{0};
  bool have_overheard_seq_{false};
  std::map<std::string, SunfreeCover *> covers_;
  bool swap_fields_{false};

  // Auto-piggyback state: fires command on next status report from target motor
  bool piggyback_armed_{false};
  uint8_t piggyback_motor_[4]{};
  uint8_t piggyback_action_{ACTION_NONE};
  uint8_t piggyback_position_{0};
  uint32_t piggyback_armed_at_{0};
  static constexpr uint32_t PIGGYBACK_TIMEOUT_MS = 300000;  // 5 minutes

  // Raw capture for replay testing
  std::vector<uint8_t> captured_raw_;
  bool have_capture_{false};

  // Diagnostic counters
  uint32_t rx_packet_count_{0};
  uint32_t rx_valid_count_{0};
  uint32_t rx_status_count_{0};
  uint32_t rx_ack_count_{0};
  uint32_t rx_cmd_count_{0};
  std::string last_rx_motor_{"none"};
  std::string last_rx_info_{"waiting"};
  std::string last_cmd_info_{"none"};
  std::string piggyback_status_{"idle"};

  static const char *action_name_(uint8_t action) {
    switch (action) {
      case ACTION_OPEN: return "OPEN";
      case ACTION_CLOSE: return "CLOSE";
      case ACTION_STOP: return "STOP";
      case ACTION_POSITION: return "POS";
      default: return "???";
    }
  }

  std::vector<uint8_t> build_command_packet_(const uint8_t *motor_id, uint8_t action,
                                              uint8_t position, uint8_t seq) {
    if (action == ACTION_STOP) {
      return build_stop_packet(this->hub_id_, motor_id, seq, this->swap_fields_);
    }
    uint8_t motor_pos = 0;
    if (action == ACTION_OPEN) motor_pos = 0x00;
    else if (action == ACTION_CLOSE) motor_pos = 0x64;
    else motor_pos = position;
    return build_position_packet(this->hub_id_, motor_id, seq, motor_pos, this->swap_fields_);
  }

  uint8_t next_seq() {
    if (this->have_overheard_seq_) {
      this->seq_ = this->overheard_seq_ + 1;
      this->have_overheard_seq_ = false;
      ESP_LOGI(TAG, "Using overheard seq+1 = 0x%02x", this->seq_);
    } else {
      this->seq_++;
    }
    return this->seq_;
  }

  void set_overheard_seq(uint8_t seq) {
    this->overheard_seq_ = seq;
    this->have_overheard_seq_ = true;
    ESP_LOGI(TAG, "Overheard seq = 0x%02x, next will be 0x%02x", seq, (uint8_t)(seq + 1));
  }

  void arm_piggyback_(const uint8_t *motor_id, uint8_t action, uint8_t position = 0) {
    memcpy(this->piggyback_motor_, motor_id, 4);
    this->piggyback_action_ = action;
    this->piggyback_position_ = position;
    this->piggyback_armed_ = true;
    this->piggyback_armed_at_ = millis();
    const char *act_name = action_name_(action);
    ESP_LOGI(TAG, "Auto-piggyback ARMED: %s for motor %s (60s timeout)",
             act_name, format_motor_id(motor_id).c_str());
    char buf[80];
    snprintf(buf, sizeof(buf), "ARMED %s %s", act_name, format_motor_id(motor_id).c_str());
    this->piggyback_status_ = buf;
  }

  bool piggyback_expired_() const {
    return this->piggyback_armed_ &&
           (millis() - this->piggyback_armed_at_ > PIGGYBACK_TIMEOUT_MS);
  }

  // Gap-free long preamble + command using CC1101 async serial mode.
  //
  // The hub's CMT2300A sends 800 bytes (160ms) of continuous 0xAA preamble
  // followed seamlessly by sync + data.  The CC1101's packet mode creates
  // 2-5ms gaps between packets that break the motor's preamble detection.
  //
  // Fix: use async serial mode to bit-bang the ENTIRE stream via GDO0
  // with absolute timestamp-based timing to maintain exact symbol rate
  // alignment with the CC1101's internal 40 kbps clock.
  void transmit_with_preamble_(std::vector<uint8_t> &pkt) {
    int pkt_sz = static_cast<int>(pkt.size());
    char hex[29 * 3 + 1];
    for (int i = 0; i < pkt_sz && i < 29; i++)
      snprintf(hex + i * 3, 4, "%02x ", pkt[i]);
    ESP_LOGI(TAG, "TX async preamble+cmd, frame (%d bytes, CRC=0x%02x): %s",
             pkt_sz, pkt_sz > 3 ? pkt[pkt_sz - 1] : 0, hex);

    // Build the entire TX stream: preamble + sync + LEN + payload + CRC + tail
    static constexpr int PREAMBLE_BYTES = 800;
    // pkt = [4A 44 LEN payload CRC]; skip first 2 bytes (4A 44 are part of sync)
    int cmd_len = static_cast<int>(pkt.size()) - 2;
    int total = PREAMBLE_BYTES + 4 + cmd_len + 10;  // +10 tail
    std::vector<uint8_t> stream(total);

    int p = 0;
    for (int i = 0; i < PREAMBLE_BYTES; i++) stream[p++] = 0xAA;
    stream[p++] = 0x53; stream[p++] = 0x52;
    stream[p++] = 0x4A; stream[p++] = 0x44;
    for (int i = 2; i < static_cast<int>(pkt.size()); i++) stream[p++] = pkt[i];
    while (p < total) stream[p++] = 0xAA;

    // Configure for TX — match hub frequency (433.920 MHz actual).
    // CC1101 crystal runs 13 kHz low, so configure 433.933 MHz.
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    this->radio_->set_frequency(433933000.0f);
    // CRITICAL: set_packet_mode(false) writes IOCFG0=0x0D so the CC1101
    // routes GDO0 for serial data instead of driving FIFO-status LOW,
    // which would create bus contention with our ESP32 GPIO output.
    this->radio_->set_packet_mode(false);
    this->radio_->begin_tx();

    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);

    ESP_LOGI(TAG, "WOR: %d bytes stream (%dms), async serial",
             total, total * 8 / 40);

    // Bit-bang using absolute timestamps for drift-free timing.
    // At 40 kbps, one bit = 25µs.  We use esp_timer_get_time() (µs resolution)
    // and busy-wait to each exact bit edge.
    int64_t t0 = esp_timer_get_time();
    int bit_idx = 0;

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    for (int b = 0; b < total; b++) {
      uint8_t byte = stream[b];
      for (int bit = 7; bit >= 0; bit--) {
        int64_t target = t0 + static_cast<int64_t>(bit_idx) * 25;
        while (esp_timer_get_time() < target) {}
        gpio_set_level(GPIO_NUM_4, (byte >> bit) & 1);
        bit_idx++;
      }
      // Yield every 256 bytes to feed watchdog (still in critical section)
      if ((b & 0xFF) == 0xFF) {
        portEXIT_CRITICAL(&mux);
        portENTER_CRITICAL(&mux);
      }
    }

    portEXIT_CRITICAL(&mux);

    // Restore packet mode RX
    this->radio_->begin_rx();
    this->radio_->set_packet_mode(true);
    this->restore_rx_();

    int64_t elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "Async TX complete (%lldms), restored RX", elapsed_us / 1000);

    // Phase 3: retransmissions using packet mode (motor should be awake now)
    this->transmit_command_only_(pkt, 2);
  }

  // Send just the command packet (no preamble), for piggyback or post-preamble.
  // Retransmissions send the identical frame (same CRC); the motor deduplicates by SEQ.
  void transmit_command_only_(std::vector<uint8_t> &pkt, int retries) {
    this->radio_->set_idle();
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    this->radio_->set_sync1(0x53);
    this->radio_->set_sync0(0x52);
    this->radio_->set_sync_mode(cc1101::SyncMode::SYNC_MODE_16_16);
    this->radio_->set_frequency(433933000.0f);
    this->radio_->set_packet_length(static_cast<uint8_t>(pkt.size()));

    for (int r = 0; r < retries; r++) {
      auto err = this->radio_->transmit_packet(pkt);
      if (err != cc1101::CC1101Error::NONE) {
        ESP_LOGW(TAG, "TX err=%d retry %d/%d", static_cast<int>(err), r, retries);
      }
    }

    // Restore RX configuration
    this->restore_rx_();
  }

  void restore_rx_() {
    this->radio_->set_idle();
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    this->radio_->set_frequency(433950000.0f);
    this->radio_->set_sync1(0xD4);
    this->radio_->set_sync0(0x92);
    this->radio_->set_packet_length(CC1101_PKT_LEN);
    this->radio_->begin_rx();
  }
};

}  // namespace sunfree_blinds
}  // namespace esphome
