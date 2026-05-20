#pragma once
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/cc1101/cc1101.h"
#include "sunfree_protocol.h"
#include <map>
#include <functional>
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "esp_timer.h"
#include "esp_random.h"

namespace esphome {
namespace sunfree_blinds {

static const char *const TAG = "sunfree";

// action codes for pending cover commands
static constexpr uint8_t ACTION_NONE = 0xFF;
static constexpr uint8_t ACTION_STOP = 0;
static constexpr uint8_t ACTION_OPEN = 1;
static constexpr uint8_t ACTION_CLOSE = 2;
static constexpr uint8_t ACTION_POSITION = 3;
// config action codes (for send_config)
static constexpr uint8_t ACTION_GOTO_FAV = 4;

class SunfreeCover;

class SunfreeHub : public Component, public api::CustomAPIDevice {
 public:
  void set_cc1101(cc1101::CC1101Component *radio) { this->radio_ = radio; }
  void set_web_base(web_server_base::WebServerBase *base) { this->web_base_ = base; }
  void set_hub_id(const std::string &id) {
    parse_motor_id(id, this->hub_id_);
    this->hub_id_from_yaml_ = true;
  }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void setup() override {
    this->init_hub_id_();
    ESP_LOGI(TAG, "Hub ID: %s", format_motor_id(this->hub_id_).c_str());
    ESP_LOGI(TAG, "Registered %d cover(s)", this->covers_.size());
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    ESP_LOGI(TAG, "CC1101: CRC and whitening disabled for Sunfree protocol");
    this->build_solicitation_frame_();

    register_service(&SunfreeHub::on_send_config_, "send_config",
                     {"motor_id", "action_type"});
    register_service(&SunfreeHub::on_group_command_, "group_command",
                     {"group", "action"});
    ESP_LOGI(TAG, "Registered services: send_config, group_command");

    if (this->web_base_) this->setup_web_();
  }

  void loop() override {
    uint32_t now = millis();

    // Follow-up retransmissions with RX gaps — mimics the Tuya hub's
    // TX/RX cycling.  Each follow-up is rebuilt with a FRESH seq so the
    // motor doesn't deduplicate.  The hub sends 6-8 TX/RX cycles ~200ms
    // apart; the motor sends STATUS after several rounds.
    if (this->followup_remaining_ > 0 && now >= this->followup_next_ms_) {
      int idx = this->followup_total_ - this->followup_remaining_;
      uint8_t seq = this->next_seq();
      auto pkt = this->build_followup_pkt_(seq);
      ESP_LOGD(TAG, "Follow-up TX %d/%d seq=0x%02x", idx + 1, this->followup_total_, seq);
      this->transmit_command_only_(pkt, 1);
      this->followup_remaining_--;
      if (this->followup_remaining_ > 0) {
        this->followup_next_ms_ = now + FOLLOWUP_GAP_MS;
      }
    }

    // Queued status poll: process one motor at a time with full TX/RX
    // cycling + cooldown before moving to the next.  The cooldown gives
    // late STATUS responses time to arrive before the next WOR blocks RX.
    if (!this->poll_queue_.empty() && this->followup_remaining_ <= 0 &&
        this->followup_group_remaining_ <= 0 &&
        now >= this->poll_cooldown_until_) {
      auto mid_str = this->poll_queue_.front();
      this->poll_queue_.erase(this->poll_queue_.begin());
      uint8_t mid[4];
      parse_motor_id(mid_str, mid);
      ESP_LOGI(TAG, "Poll queue: sending STOP to %s (%d remaining)",
               mid_str.c_str(), static_cast<int>(this->poll_queue_.size()));
      this->request_status(mid);
      this->poll_cooldown_until_ = now + POLL_COOLDOWN_MS;
    }

    // Group follow-ups: retransmit all group commands per round
    if (this->followup_group_remaining_ > 0 && this->followup_remaining_ <= 0 &&
        now >= this->followup_next_ms_) {
      ESP_LOGD(TAG, "Group follow-up %d/%d (%d motors)",
               FOLLOWUP_COUNT - this->followup_group_remaining_ + 1,
               FOLLOWUP_COUNT,
               static_cast<int>(this->followup_group_pkts_.size()));
      for (auto &pkt : this->followup_group_pkts_) {
        this->transmit_command_only_(pkt, 1);
      }
      this->followup_group_remaining_--;
      if (this->followup_group_remaining_ > 0) {
        this->followup_next_ms_ = now + FOLLOWUP_GAP_MS;
      }
    }

    if (!this->scan_active_) return;

    // Check scan timeout
    if (now - this->scan_start_ms_ > SCAN_TIMEOUT_MS) {
      this->scan_active_ = false;
      this->pairing_status_ = "SCAN TIMEOUT";
      ESP_LOGW(TAG, "Pairing scan timed out after %ds", SCAN_TIMEOUT_MS / 1000);
      return;
    }

    // Send discovery packet every SOLICIT_INTERVAL_MS while scanning.
    // The motor enters LISTEN mode after M button press (~10s window).
    // The hub (us) must be actively sending for the motor to hear us.
    if (now - this->last_solicit_ms_ >= SOLICIT_INTERVAL_MS) {
      this->last_solicit_ms_ = now;
      this->solicit_count_++;
      // Rebuild with fresh seq each time (motor may deduplicate by seq)
      this->build_solicitation_frame_();
      ESP_LOGI(TAG, "Pairing TX #%d seq=0x%02x (t=%ds)", this->solicit_count_,
               this->seq_, (now - this->scan_start_ms_) / 1000);
      this->transmit_with_preamble_(this->solicit_frame_);
    }
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
    this->schedule_followups_(motor_id, action, position, true);

    // Also arm auto-piggyback as fallback for deep-sleeping motors
    this->arm_piggyback_(motor_id, action, position);
  }

  void add_group(const std::string &name, std::vector<std::string> motor_ids) {
    this->groups_[name] = std::move(motor_ids);
    ESP_LOGI(TAG, "Group '%s' added with %d motors", name.c_str(),
             static_cast<int>(this->groups_[name].size()));
  }

  const std::map<std::string, std::vector<std::string>> &get_groups() const {
    return this->groups_;
  }

  // Send a command to a named group (single WOR preamble, all motors move together).
  // Use group="" for all motors.
  // Implemented in sunfree_cover.h where SunfreeCover is fully defined.
  void send_group_command(const std::string &group, uint8_t action, uint8_t position = 0);

  // Send a configuration command (limits, direction, favourite, etc.)
  // Uses n=4 XXTEA encryption with arbitrary action+value bytes.
  void send_config(const uint8_t *motor_id, uint8_t action, uint8_t value) {
    uint8_t seq = this->next_seq();
    std::vector<uint8_t> pkt = build_n4_command(
        this->hub_id_, motor_id, seq, action, value, this->swap_fields_);
    ESP_LOGI(TAG, "TX CONFIG action=0x%02x value=0x%02x seq=0x%02x motor=%s",
             action, value, seq, format_motor_id(motor_id).c_str());
    this->transmit_with_preamble_(pkt);
    this->schedule_followups_(motor_id, action, value);
  }

  // Poll a motor for status (battery + position).
  // Sends a STOP command — harmless if motor is already stopped, but
  // triggers the full TX/RX cycling that elicits STATUS reports.
  // (0x2A STATUS_QUERY only gets ACKs; STOP triggers actual STATUS.)
  void request_status(const uint8_t *motor_id) {
    this->send_command(motor_id, ACTION_STOP);
  }

  float get_last_rssi() const { return this->last_rssi_; }

 protected:
  void on_send_config_(std::string motor_id, std::string action_type) {
    // "request_status" with "all" queues STOP poll for every motor
    if (action_type == "request_status" && motor_id == "all") {
      this->on_request_status_all_();
      return;
    }

    uint8_t mid[4];
    if (motor_id.length() != 8) {
      ESP_LOGW(TAG, "send_config: motor_id must be 8 hex chars, got '%s'", motor_id.c_str());
      return;
    }
    parse_motor_id(motor_id, mid);

    uint8_t action, value;
    if (action_type == "direction_forward") {
      action = static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION);
      value = DIR_FORWARD;
    } else if (action_type == "direction_reverse") {
      action = static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION);
      value = DIR_REVERSE;
    } else if (action_type == "set_open_limit") {
      action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
      value = LIMIT_OPEN;
    } else if (action_type == "set_close_limit") {
      action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
      value = LIMIT_CLOSE;
    } else if (action_type == "save_favourite") {
      action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
      value = LIMIT_FAVOURITE;
    } else if (action_type == "goto_favourite") {
      action = static_cast<uint8_t>(SunfreeCmd::GOTO_FAVOURITE);
      value = GOTO_FAV_VALUE;
    } else if (action_type == "request_status") {
      this->request_status(mid);
      return;
    } else {
      ESP_LOGW(TAG, "send_config: unknown action_type '%s'", action_type.c_str());
      return;
    }

    ESP_LOGI(TAG, "Service send_config: motor=%s action=%s (0x%02X/0x%02X)",
             motor_id.c_str(), action_type.c_str(), action, value);
    this->send_config(mid, action, value);
  }

  void on_request_status_all_() {
    this->poll_queue_.clear();
    this->poll_cooldown_until_ = 0;
    for (auto &kv : this->covers_) {
      this->poll_queue_.push_back(kv.first);
    }
    ESP_LOGI(TAG, "Queued STOP poll for %d motors",
             static_cast<int>(this->poll_queue_.size()));
  }

  void on_group_command_(std::string group, std::string action_str) {
    uint8_t action;
    uint8_t position = 0;
    if (action_str == "open") {
      action = ACTION_OPEN;
    } else if (action_str == "close") {
      action = ACTION_CLOSE;
    } else if (action_str == "stop") {
      action = ACTION_STOP;
    } else if (action_str.substr(0, 9) == "position_") {
      action = ACTION_POSITION;
      position = static_cast<uint8_t>(std::atoi(action_str.substr(9).c_str()));
    } else {
      ESP_LOGW(TAG, "group_command: unknown action '%s'", action_str.c_str());
      return;
    }
    ESP_LOGI(TAG, "Service group_command: group='%s' action=%s", group.c_str(), action_str.c_str());
    this->send_group_command(group, action, position);
  }

 public:
  // Quick TX for piggyback: motor is already awake, skip preamble
  void send_quick(const uint8_t *motor_id, uint8_t action, uint8_t position = 0) {
    uint8_t seq = this->next_seq();
    std::vector<uint8_t> pkt = this->build_command_packet_(motor_id, action, position, seq);
    const char *act_name = action_name_(action);
    ESP_LOGI(TAG, "QUICK TX %s seq=0x%02x motor=%s", act_name, seq,
             format_motor_id(motor_id).c_str());
    this->transmit_command_only_(pkt, 3);
  }

  std::string get_hub_id_str() const { return format_motor_id(this->hub_id_); }
  std::string get_motors_json();  // implemented in sunfree_web.h

  uint32_t get_rx_packet_count() const { return this->rx_packet_count_; }
  uint32_t get_rx_valid_count() const { return this->rx_valid_count_; }
  uint32_t get_rx_status_count() const { return this->rx_status_count_; }
  uint32_t get_rx_ack_count() const { return this->rx_ack_count_; }
  uint32_t get_rx_cmd_count() const { return this->rx_cmd_count_; }
  const std::string &get_last_rx_motor() const { return this->last_rx_motor_; }
  const std::string &get_last_rx_info() const { return this->last_rx_info_; }
  const std::string &get_piggyback_status() const { return this->piggyback_status_; }

  void on_cc1101_packet(const std::vector<uint8_t> &data, float rssi);

  // Scan / pairing
  void start_scan() {
    this->scan_active_ = true;
    this->scan_start_ms_ = millis();
    this->last_solicit_ms_ = 0;  // force immediate first TX
    this->solicit_count_ = 0;
    this->pairing_status_ = "SCANNING+TX...";
    ESP_LOGI(TAG, "Pairing scan STARTED: sending solicitation every %dms for %ds",
             SOLICIT_INTERVAL_MS, SCAN_TIMEOUT_MS / 1000);
  }

  void stop_scan() {
    this->scan_active_ = false;
    this->pairing_status_ = "STOPPED";
    ESP_LOGI(TAG, "Pairing scan STOPPED");
  }

  bool is_scanning() const { return this->scan_active_; }
  const std::string &get_pairing_status() const { return this->pairing_status_; }
  uint32_t get_rx_beacon_count() const { return this->rx_beacon_count_; }

  // Proactive pairing: now just an alias for start_scan().
  // The loop() continuously sends solicitation + listens during scan.
  void send_pairing_solicitation() {
    this->start_scan();
  }

  void send_pairing_response(const uint8_t *resp_dec16) {
    const uint8_t *motor_addr = resp_dec16 + 3;
    char mid[20];
    snprintf(mid, sizeof(mid), "%02x%02x%02x%02x",
             motor_addr[0], motor_addr[1], motor_addr[2], motor_addr[3]);
    std::string mid_str(mid);
    ESP_LOGI(TAG, "Motor paired! ID=%s", mid);
    this->pairing_status_ = "PAIRED: " + mid_str;

    bool already_listed = false;
    for (auto &d : this->discovered_ids_) {
      if (d == mid_str) { already_listed = true; break; }
    }
    if (!already_listed) {
      this->discovered_ids_.push_back(mid_str);
      ESP_LOGI(TAG, "New motor discovered: %s (total discovered: %d)",
               mid, static_cast<int>(this->discovered_ids_.size()));
    }
  }

  const std::vector<std::string> &get_discovered_ids() const {
    return this->discovered_ids_;
  }

  std::string get_discovered_ids_str() const {
    if (this->discovered_ids_.empty()) return "none";
    std::string result;
    for (size_t i = 0; i < this->discovered_ids_.size(); i++) {
      if (i) result += ", ";
      result += this->discovered_ids_[i];
    }
    return result;
  }

 protected:
  cc1101::CC1101Component *radio_{nullptr};
  web_server_base::WebServerBase *web_base_{nullptr};
  uint8_t hub_id_[4]{};
  bool hub_id_from_yaml_{false};
  uint8_t seq_{0x80};
  uint8_t overheard_seq_{0};
  bool have_overheard_seq_{false};
  std::map<std::string, SunfreeCover *> covers_;
  std::map<std::string, std::vector<std::string>> groups_;
  bool swap_fields_{false};

  // Auto-piggyback state: fires command on next status report from target motor
  bool piggyback_armed_{false};
  uint8_t piggyback_motor_[4]{};
  uint8_t piggyback_action_{ACTION_NONE};
  uint8_t piggyback_position_{0};
  uint32_t piggyback_armed_at_{0};
  static constexpr uint32_t PIGGYBACK_TIMEOUT_MS = 300000;  // 5 minutes

  // Scan / pairing state
  bool scan_active_{false};
  uint32_t scan_start_ms_{0};
  uint32_t last_solicit_ms_{0};
  uint32_t solicit_count_{0};
  std::vector<uint8_t> solicit_frame_;
  static constexpr uint32_t SCAN_TIMEOUT_MS = 120000;    // 120 seconds
  static constexpr uint32_t SOLICIT_INTERVAL_MS = 1500;   // TX every 1.5s (160ms TX + listen gap)
  std::string pairing_status_{"idle"};
  std::vector<std::string> discovered_ids_;

  // Follow-up retransmission state (TX/RX cycling like Tuya hub).
  // Each follow-up is rebuilt with a fresh seq so the motor doesn't
  // deduplicate.  We store the context (motor_id, action, value) to
  // rebuild packets on each retransmission.
  uint8_t followup_motor_id_[4]{};
  uint8_t followup_action_{0};
  uint8_t followup_value_{0};
  bool followup_is_command_{false};
  std::vector<std::vector<uint8_t>> followup_group_pkts_;
  uint32_t followup_next_ms_{0};
  int followup_remaining_{0};
  int followup_total_{0};
  int followup_group_remaining_{0};
  static constexpr uint32_t FOLLOWUP_GAP_MS = 200;
  static constexpr int FOLLOWUP_COUNT = 6;

  // Queued status poll — process one motor at a time with full TX/RX
  // cycling between each, so motors get proper follow-ups.
  std::vector<std::string> poll_queue_;
  uint32_t poll_cooldown_until_{0};
  static constexpr uint32_t POLL_COOLDOWN_MS = 500;


  // Diagnostic counters
  uint32_t rx_packet_count_{0};
  uint32_t rx_valid_count_{0};
  uint32_t rx_status_count_{0};
  uint32_t rx_ack_count_{0};
  uint32_t rx_cmd_count_{0};
  uint32_t rx_beacon_count_{0};
  float last_rssi_{0.0f};
  std::string last_rx_motor_{"none"};
  std::string last_rx_info_{"waiting"};
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

  // Rebuild follow-up packet with a fresh seq to avoid motor deduplication.
  std::vector<uint8_t> build_followup_pkt_(uint8_t seq) {
    if (this->followup_is_command_) {
      return this->build_command_packet_(this->followup_motor_id_,
                                          this->followup_action_,
                                          this->followup_value_, seq);
    }
    return build_n4_command(this->hub_id_, this->followup_motor_id_, seq,
                            this->followup_action_, this->followup_value_,
                            this->swap_fields_);
  }

  // Send ACK back to motor after receiving its STATUS report.
  // The Tuya hub does this; the motor may require it to send further reports.
  void send_ack_to_motor_(const uint8_t *motor_id, uint8_t seq) {
    std::vector<uint8_t> pkt = build_ack_packet(
        this->hub_id_, motor_id, seq, 0x00, this->swap_fields_);
    ESP_LOGD(TAG, "TX ACK→motor seq=0x%02x motor=%s",
             seq, format_motor_id(motor_id).c_str());
    this->transmit_command_only_(pkt, 1);
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
  // Static TX buffer avoids heap allocation during the critical-section
  // bit-bang, preventing heap corruption that crashes the SPI driver.
  static constexpr int TX_BUF_MAX_ = 1100;
  uint8_t tx_buf_[TX_BUF_MAX_];

  void bitbang_tx_(int total) {
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    // TX at 433.933 MHz during pairing (motor WOR listens here),
    // 433.950 MHz for normal commands (motor is already awake).
    float tx_freq = this->scan_active_ ? 433933000.0f : 433950000.0f;
    this->radio_->set_frequency(tx_freq);
    this->radio_->set_packet_mode(false);
    this->radio_->begin_tx();

    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);

    ESP_LOGD(TAG, "WOR: %d bytes (%dms)", total, total * 8 / 40);

    int64_t t0 = esp_timer_get_time();
    int bit_idx = 0;

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);

    for (int b = 0; b < total; b++) {
      uint8_t byte = this->tx_buf_[b];
      for (int bit = 7; bit >= 0; bit--) {
        int64_t target = t0 + static_cast<int64_t>(bit_idx) * 25;
        while (esp_timer_get_time() < target) {}
        gpio_set_level(GPIO_NUM_4, (byte >> bit) & 1);
        bit_idx++;
      }
      if ((b & 0xFF) == 0xFF) {
        portEXIT_CRITICAL(&mux);
        portENTER_CRITICAL(&mux);
      }
    }

    portEXIT_CRITICAL(&mux);

    // Restore GDO0 as input before touching SPI again
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_INPUT);

    // Get back into RX as fast as possible -- the motor's pairing response
    // is a short burst that can arrive within milliseconds of our TX ending.
    this->radio_->begin_rx();
    this->radio_->set_packet_mode(true);
    this->restore_rx_();

    int64_t elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGD(TAG, "TX done (%lldms) f=%.3fMHz", elapsed_us / 1000, tx_freq / 1e6);
  }

  void transmit_with_preamble_(std::vector<uint8_t> &pkt) {
    ESP_LOGD(TAG, "TX preamble+cmd (%d bytes)", static_cast<int>(pkt.size()));

    static constexpr int PREAMBLE_BYTES = 800;  // 160ms WOR preamble (matches original Tuya hub)
    int cmd_len = static_cast<int>(pkt.size()) - 2;
    int total = PREAMBLE_BYTES + 4 + cmd_len + 10;
    if (total > TX_BUF_MAX_) {
      ESP_LOGE(TAG, "TX stream too large: %d > %d", total, TX_BUF_MAX_);
      return;
    }

    int p = 0;
    memset(this->tx_buf_, 0xAA, total);
    p = PREAMBLE_BYTES;
    this->tx_buf_[p++] = 0x53; this->tx_buf_[p++] = 0x52;
    this->tx_buf_[p++] = 0x4A; this->tx_buf_[p++] = 0x44;
    for (int i = 2; i < static_cast<int>(pkt.size()); i++) this->tx_buf_[p++] = pkt[i];

    this->bitbang_tx_(total);
  }

  // Schedule follow-up retransmissions via loop().  Each follow-up is
  // rebuilt with a fresh seq so the motor doesn't deduplicate by seq.
  // is_command=true for movement commands (OPEN/CLOSE/STOP/POS),
  // false for config/query commands (0x2A, 0x22, etc.)
  void schedule_followups_(const uint8_t *motor_id, uint8_t action,
                            uint8_t value, bool is_command = false) {
    if (this->scan_active_) return;
    memcpy(this->followup_motor_id_, motor_id, 4);
    this->followup_action_ = action;
    this->followup_value_ = value;
    this->followup_is_command_ = is_command;
    this->followup_remaining_ = FOLLOWUP_COUNT;
    this->followup_total_ = FOLLOWUP_COUNT;
    this->followup_next_ms_ = millis() + FOLLOWUP_GAP_MS;
    ESP_LOGD(TAG, "Scheduled %d follow-ups", FOLLOWUP_COUNT);
  }

  // Single preamble followed by multiple command packets, each with its own
  // sync word.  All motors are awake from the WOR preamble so they all
  // receive their command within milliseconds of each other.
  void transmit_group_with_preamble_(std::vector<std::vector<uint8_t>> &pkts) {
    static constexpr int PREAMBLE_BYTES = 800;  // 160ms WOR preamble (matches original Tuya hub)
    static constexpr int GAP_BYTES = 20;

    int payload_total = 0;
    for (auto &pkt : pkts) {
      payload_total += 4 + (static_cast<int>(pkt.size()) - 2) + GAP_BYTES;
    }
    int total = PREAMBLE_BYTES + payload_total + 10;
    if (total > TX_BUF_MAX_) {
      ESP_LOGE(TAG, "GROUP TX stream too large: %d > %d", total, TX_BUF_MAX_);
      return;
    }

    memset(this->tx_buf_, 0xAA, total);
    int p = PREAMBLE_BYTES;
    for (auto &pkt : pkts) {
      this->tx_buf_[p++] = 0x53; this->tx_buf_[p++] = 0x52;
      this->tx_buf_[p++] = 0x4A; this->tx_buf_[p++] = 0x44;
      for (int i = 2; i < static_cast<int>(pkt.size()); i++) this->tx_buf_[p++] = pkt[i];
      p += GAP_BYTES;  // already 0xAA from memset
    }

    ESP_LOGD(TAG, "GROUP WOR: %d bytes (%dms), %d motors",
             total, total * 8 / 40, static_cast<int>(pkts.size()));

    this->bitbang_tx_(total);

    // Schedule follow-ups for all motors in the group.  The first
    // follow-up fires after FOLLOWUP_GAP_MS; it retransmits all
    // group commands so each motor gets TX/RX cycling for STATUS.
    if (!pkts.empty()) {
      this->followup_group_pkts_ = pkts;
      this->followup_group_remaining_ = FOLLOWUP_COUNT;
      this->followup_next_ms_ = millis() + FOLLOWUP_GAP_MS;
      ESP_LOGD(TAG, "GROUP: scheduled %d follow-ups for %d motors",
               FOLLOWUP_COUNT, static_cast<int>(pkts.size()));
    }
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
    this->radio_->set_frequency(433950000.0f);
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

  void init_hub_id_() {
    if (this->hub_id_from_yaml_) {
      ESP_LOGI(TAG, "Hub ID from YAML: %s", format_motor_id(this->hub_id_).c_str());
      return;
    }

    // Try loading from NVS
    uint32_t stored_id = 0;
    auto pref = global_preferences->make_preference<uint32_t>(fnv1_hash("sunfree_hub_id"));
    if (pref.load(&stored_id) && stored_id != 0) {
      memcpy(this->hub_id_, &stored_id, 4);
      ESP_LOGI(TAG, "Hub ID from NVS: %s", format_motor_id(this->hub_id_).c_str());
      return;
    }

    // Generate random hub ID
    stored_id = esp_random();
    while (stored_id == 0) stored_id = esp_random();
    memcpy(this->hub_id_, &stored_id, 4);
    pref.save(&stored_id);
    global_preferences->sync();
    ESP_LOGI(TAG, "Hub ID generated: %s (saved to NVS)", format_motor_id(this->hub_id_).c_str());
  }

  // Build the binary discovery frame (re-built each TX with incrementing seq).
  void build_solicitation_frame_() {
    this->solicit_frame_ = build_discovery_packet(this->hub_id_, this->next_seq());
  }

  void restore_rx_() {
    this->radio_->set_idle();
    this->radio_->set_crc_enable(false);
    this->radio_->set_whitening(false);
    // Always use 433.950 MHz for RX — RTL-SDR confirmed motor responds
    // at ~433.935 MHz regardless of solicitation frequency (within the
    // 203 kHz RX filter bandwidth centered on 433.950)
    this->radio_->set_frequency(433950000.0f);
    this->radio_->set_sync_mode(cc1101::SyncMode::SYNC_MODE_16_16);
    this->radio_->set_sync1(0xD4);
    this->radio_->set_sync0(0x92);
    this->radio_->set_packet_length(CC1101_PKT_LEN);
    this->radio_->begin_rx();
  }

  void setup_web_();  // implemented in sunfree_web.h
};

}  // namespace sunfree_blinds
}  // namespace esphome
