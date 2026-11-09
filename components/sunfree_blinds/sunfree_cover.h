#pragma once
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/sensor/sensor.h"
#include "sunfree_hub.h"
#include "sunfree_protocol.h"

namespace esphome {
namespace sunfree_blinds {

class SunfreeCover : public cover::Cover, public Component {
 public:
  void set_hub(SunfreeHub *hub) { this->hub_ = hub; }
  void set_motor_id(const std::string &id) {
    if (!parse_motor_id(id, this->motor_id_)) {
      ESP_LOGE("sunfree", "Invalid motor_id '%s' — must be 8 hex chars", id.c_str());
      return;
    }
    this->motor_id_str_ = id;
  }
  void set_invert_position(bool invert) { this->invert_position_ = invert; }
  void set_battery_sensor(sensor::Sensor *sensor) { this->battery_sensor_ = sensor; }
  bool get_invert_position() const { return this->invert_position_; }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI - 1; }

  void setup() override {
    this->hub_->register_cover(this);
  }

  void dump_config() override {
    LOG_COVER("", "Sunfree Cover", this);
    ESP_LOGCONFIG("sunfree", "  Motor ID: %s", this->motor_id_str_.c_str());
    ESP_LOGCONFIG("sunfree", "  Invert position: %s", YESNO(this->invert_position_));
  }

  cover::CoverTraits get_traits() override {
    auto traits = cover::CoverTraits();
    traits.set_supports_position(true);
    traits.set_supports_stop(true);
    traits.set_supports_toggle(false);
    traits.set_is_assumed_state(false);
    return traits;
  }

  void control(const cover::CoverCall &call) override {
    if (call.get_stop()) {
      this->hub_->send_command(this->motor_id_, ACTION_STOP);
      return;
    }
    if (call.get_position().has_value()) {
      float target = *call.get_position();
      // HA: 0.0=closed, 1.0=open.
      // Standard motor: 0=open, 100=closed.
      // Inverted motor: 0=closed, 100=open.
      uint8_t motor_pos;
      if (this->invert_position_) {
        motor_pos = static_cast<uint8_t>(target * 100.0f);
      } else {
        motor_pos = static_cast<uint8_t>((1.0f - target) * 100.0f);
      }
      uint8_t action;
      if (motor_pos == 0) {
        action = ACTION_OPEN;
      } else if (motor_pos >= 100) {
        action = ACTION_CLOSE;
      } else {
        action = ACTION_POSITION;
      }
      // OPEN/CLOSE carry no position byte on air — keep sending 0 there.
      this->hub_->send_command(this->motor_id_, action,
                               action == ACTION_POSITION ? motor_pos : 0);
      this->apply_command(action, motor_pos);
    }
  }

  // Watchdog for a lost final STATUS.  Observed in HA history: motors
  // report STATUS ~2s after accepting a command — already carrying the
  // *target* position and a moving state — and report again (state=0) on
  // arrival.
  // That arrival report is sometimes never received (radio busy with the
  // next group burst, or simply lost), and nothing else ever moves the
  // cover back to IDLE: covers sat in "opening"/"closing" for 5+ hours,
  // so any HA wait_template on "not opening/closing" times out instead of
  // completing.  After SETTLE_TIMEOUT_MS with no fresh motion report,
  // assume the motor has arrived.  Position is left alone — the last
  // STATUS (or the commanded target) is still the best estimate.
  // We deliberately do NOT send a status poll here: the poll is a STOP,
  // which would halt a motor that is in fact still travelling.
  void loop() override {
    if (this->current_operation == cover::COVER_OPERATION_IDLE) return;
    if (millis() - this->last_motion_ms_ < SETTLE_TIMEOUT_MS) return;
    ESP_LOGW("sunfree", "%s: no arrival STATUS within %us — marking idle at %d%%",
             this->motor_id_str_.c_str(), static_cast<unsigned>(SETTLE_TIMEOUT_MS / 1000),
             static_cast<int>(this->position * 100));
    this->current_operation = cover::COVER_OPERATION_IDLE;
    this->publish_state();
  }

  // Record a command that was just transmitted to this motor, so HA shows
  // the target until the motor's own STATUS replaces it.  `action` and
  // `motor_pos` are in MOTOR space, exactly as sent on air (i.e. after any
  // invert_position flip), and are mapped back to HA space with the same
  // conversion on_status() uses.
  //
  // Group commands used to compute this in HA space themselves and got the
  // inverted case backwards: the action was flipped for the motor but the
  // published target was not, so every inverted motor briefly reported the
  // mirror of its target (position_25 showed 25% while heading to 75%,
  // "open" showed closed).  STATUS corrected it seconds later, but anything
  // reading current_position in that window — e.g. a script planning the
  // next move — planned from the wrong side of the window.
  void apply_command(uint8_t action, uint8_t motor_pos) {
    if (action == ACTION_STOP) {
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->publish_state();
      return;
    }
    if (action == ACTION_OPEN) {
      motor_pos = 0;
    } else if (action == ACTION_CLOSE) {
      motor_pos = 100;
    }
    float prev = this->position;
    float target = this->motor_to_ha_(motor_pos);
    this->position = target;
    if (target > prev) {
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else if (target < prev) {
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    }
    // target == prev: already there, the motor won't move — leave the
    // operation as it was rather than claiming a "closing" that never ends.
    this->last_motion_ms_ = millis();
    this->publish_state();
  }

  const uint8_t *get_motor_id() const { return this->motor_id_; }
  const std::string &get_motor_id_str() const { return this->motor_id_str_; }
  sensor::Sensor *get_battery_sensor() const { return this->battery_sensor_; }

  void on_ack(const SunfreeResponse &resp) {
    ESP_LOGI("sunfree", "ACK for %s flags=0x%02x", this->motor_id_str_.c_str(), resp.ack_flags);
    if (resp.ack_flags == 0x40 &&
        this->current_operation != cover::COVER_OPERATION_IDLE) {
      this->current_operation = cover::COVER_OPERATION_IDLE;
      this->publish_state();
    }
  }

  void on_status(uint8_t state, uint8_t motor_position, uint8_t battery) {
    float ha_pos = this->motor_to_ha_(motor_position);
    this->position = ha_pos;

    switch (state) {
      case 0:
        this->current_operation = cover::COVER_OPERATION_IDLE;
        break;
      case 1:
        this->current_operation = cover::COVER_OPERATION_CLOSING;
        this->last_motion_ms_ = millis();
        break;
      case 2:
        this->current_operation = cover::COVER_OPERATION_OPENING;
        this->last_motion_ms_ = millis();
        break;
    }
    this->publish_state();

    if (this->battery_sensor_ != nullptr) {
      this->battery_sensor_->publish_state(battery);
    }

    ESP_LOGD("sunfree", "Status %s: state=%d pos=%d(%d%%) bat=%d%%",
             this->motor_id_str_.c_str(), state, motor_position,
             static_cast<int>(ha_pos * 100), battery);
  }

 protected:
  // Motor space (0..100) → HA space (0.0=closed, 1.0=open).
  // Standard motor: 0=open, 100=closed.  Inverted motor: 0=closed, 100=open.
  float motor_to_ha_(uint8_t motor_pos) const {
    if (this->invert_position_) return motor_pos / 100.0f;
    return (100.0f - motor_pos) / 100.0f;
  }

  // A full travel takes ~35s on these rollers; 60s leaves room for a slow
  // motor on a low battery without leaving HA waiting much past arrival.
  static constexpr uint32_t SETTLE_TIMEOUT_MS = 60000;

  SunfreeHub *hub_{nullptr};
  uint8_t motor_id_[4]{};
  std::string motor_id_str_;
  bool invert_position_{false};
  sensor::Sensor *battery_sensor_{nullptr};
  uint32_t last_motion_ms_{0};
};

// Implementation of hub method that depends on SunfreeCover
inline void SunfreeHub::register_cover(SunfreeCover *cover) {
  this->covers_[cover->get_motor_id_str()] = cover;
}

inline void SunfreeHub::on_cc1101_packet(const std::vector<uint8_t> &data, float rssi) {
  this->rx_packet_count_++;

  // Expire stale piggyback
  if (this->piggyback_expired_()) {
    this->piggyback_armed_ = false;
    this->piggyback_status_ = "TIMEOUT";
    ESP_LOGW(TAG, "Piggyback expired after %dms", PIGGYBACK_TIMEOUT_MS);
  }

  // Scan timeout is now handled in loop()

  int n = data.size() < 29 ? data.size() : 29;
  char hex[29 * 3 + 1];
  for (int i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02x ", data[i]);

  // During scan, log ALL strong packets at INFO level to detect motor responses
  // even if the first byte isn't 0x91 (could indicate frequency/sync issues)
  if (this->scan_active_ && rssi > -90.0f) {
    ESP_LOGW(TAG, "SCAN STRONG RX #%u (%d bytes, rssi=%.0f): %s",
             this->rx_packet_count_, data.size(), rssi, hex);
  } else {
    ESP_LOGD(TAG, "RX #%u raw (%d bytes, rssi=%.0f): %s",
             this->rx_packet_count_, data.size(), rssi, hex);
  }

  if (data.size() < 2 || data[0] != 0x91) return;

  int pkt_len = d492_get_len(data.data(), data.size());
  if (pkt_len == 0) {
    ESP_LOGW(TAG, "RX 0x91 unknown type=0x%02x rssi=%.0f", data[1], rssi);
    return;
  }

  this->rx_valid_count_++;

  if (pkt_len == 12) {
    // ACK from motor
    this->rx_ack_count_++;
    SunfreeResponse resp{};
    if (parse_ack(data.data(), data.size(), resp) && resp.valid) {
      std::string mid = format_motor_id(resp.motor_id);
      std::string hid = format_motor_id(resp.hub_id);
      this->last_rx_motor_ = mid;
      char buf[80];
      snprintf(buf, sizeof(buf), "ACK %s→%s flags=0x%02x",
               mid.c_str(), hid.c_str(), resp.ack_flags);
      this->last_rx_info_ = buf;
      ESP_LOGI(TAG, "RX ACK from %s flags=0x%02x rssi=%.0f",
               mid.c_str(), resp.ack_flags, rssi);

      auto it = this->covers_.find(mid);
      if (it != this->covers_.end()) {
        it->second->on_ack(resp);
      }

      // Motor confirmed receipt — cancel any pending retransmission.
      // Only if it ACKed *us*: when coexisting with a Tuya hub, a motor
      // ACKing the Tuya hub says nothing about our command being heard.
      if (memcmp(resp.hub_id, this->hub_id_, 4) == 0) {
        this->clear_ack_watch_(resp.motor_id);
      }

      // Fire piggyback on ACK from target motor — the motor just finished
      // its ACK TX and is transitioning back to RX for follow-up packets.
      // Verify both motor_id AND hub_id to avoid triggering on hub→motor
      // follow-up packets that happen to parse as ACK format.
      if (this->piggyback_armed_ &&
          memcmp(resp.motor_id, this->piggyback_motor_, 4) == 0 &&
          memcmp(resp.hub_id, this->hub_id_, 4) == 0) {
        this->piggyback_armed_ = false;
        const char *act_name = action_name_(this->piggyback_action_);
        ESP_LOGI(TAG, "Auto-piggyback FIRED %s on ACK(0x%02x) from %s (20ms settle)",
                 act_name, resp.ack_flags, mid.c_str());
        char pbuf[80];
        snprintf(pbuf, sizeof(pbuf), "FIRED %s on ACK(0x%02x)", act_name, resp.ack_flags);
        this->piggyback_status_ = pbuf;
        delay(20);
        this->send_quick(this->piggyback_motor_, this->piggyback_action_,
                         this->piggyback_position_);
      }
    } else {
      this->last_rx_info_ = "ACK parse_fail";
    }

  } else if (pkt_len == 16) {
    // Command (overheard from Tuya hub or other device)
    this->rx_cmd_count_++;
    ESP_LOGD(TAG, "RX CMD frame (%d bytes)", static_cast<int>(data.size()));
    SunfreePacket pkt{};
    if (parse_command(data.data(), data.size(), pkt) && pkt.valid) {
      std::string mid = format_motor_id(pkt.motor_id);
      std::string hid = format_motor_id(pkt.hub_id);
      this->last_rx_motor_ = mid;
      char buf[120];
      snprintf(buf, sizeof(buf), "CMD h=%s m=%s a=%d v=%d s=0x%02x",
               hid.c_str(), mid.c_str(), static_cast<int>(pkt.action),
               pkt.value, pkt.seq);
      this->last_rx_info_ = buf;
      this->set_overheard_seq(pkt.seq);
    } else {
      uint8_t extracted[16];
      d492_extract_payload(data.data(), data.size(), extracted, 16);

      // Check for beacon: byte 14 = 0xEE in RAW cleartext (n=3 encryption,
      // bytes 12-15 are unencrypted — like STOP commands) OR in n=4 decrypt
      bool is_beacon = (extracted[14] == 0xEE);

      uint32_t w4[4];
      memcpy(w4, extracted, 16);
      xxtea_decrypt(w4, 4, SUNFREE_KEY);
      uint8_t d4[16];
      memcpy(d4, w4, 16);

      if (!is_beacon) is_beacon = (d4[14] == 0xEE);

      char hx[16 * 3 + 1];
      for (int i = 0; i < 16; i++) snprintf(hx + i * 3, 4, "%02x ", d4[i]);

      if (is_beacon) {
        this->rx_beacon_count_++;
        this->rx_cmd_count_--;

        char raw_hx[16 * 3 + 1];
        for (int i = 0; i < 16; i++) snprintf(raw_hx + i * 3, 4, "%02x ", extracted[i]);
        ESP_LOGI(TAG, "PAIRING BEACON: raw=%s dec4=%s rssi=%.0f", raw_hx, hx, rssi);
        ESP_LOGI(TAG, "BEACON raw[14]=0x%02x dec4[14]=0x%02x", extracted[14], d4[14]);

        // Also try n=3 decrypt (first 12 bytes only) for proper address extraction
        uint8_t d3[16];
        memcpy(d3, extracted, 16);
        uint32_t w3[3];
        memcpy(w3, d3, 12);
        xxtea_decrypt(w3, 3, SUNFREE_KEY);
        memcpy(d3, w3, 12);  // bytes 12-15 stay as cleartext
        char d3_hx[16 * 3 + 1];
        for (int i = 0; i < 16; i++) snprintf(d3_hx + i * 3, 4, "%02x ", d3[i]);
        ESP_LOGI(TAG, "BEACON dec3=%s", d3_hx);

        char buf[120];
        snprintf(buf, sizeof(buf), "BEACON raw14=0x%02x dec4=%s", extracted[14], hx);
        this->last_rx_info_ = buf;

        if (this->scan_active_) {
          this->pairing_status_ = "BEACON FOUND";
          ESP_LOGI(TAG, "PAIRING: beacon found! Sending response...");
          // Try n=3 decrypted data for address (bytes 3-6)
          this->send_pairing_response(d3);
        }
      } else {
        ESP_LOGW(TAG, "CMD parse_fail dec4: %s rssi=%.0f", hx, rssi);
        char buf[120];
        snprintf(buf, sizeof(buf), "CMD fail dec=%s", hx);
        this->last_rx_info_ = buf;
      }
    }

    // Don't fire piggyback on CMD — motor is busy processing the hub's
    // command and about to TX its ACK.  Wait for the ACK instead (the motor
    // transitions back to RX after its ACK, giving us a clean window).

  } else if (pkt_len == 24) {
    // Status report from motor
    this->rx_status_count_++;
    SunfreeResponse resp{};
    if (parse_status(data.data(), data.size(), resp) && resp.valid) {
      std::string mid = format_motor_id(resp.motor_id);
      this->last_rx_motor_ = mid;
      char buf[80];
      snprintf(buf, sizeof(buf), "STATUS %s pos=%d bat=%d%% state=%d",
               mid.c_str(), resp.position, resp.battery, resp.state);
      this->last_rx_info_ = buf;
      ESP_LOGI(TAG, "RX %s rssi=%.0f", buf, rssi);

      auto it = this->covers_.find(mid);
      if (it != this->covers_.end()) {
        it->second->on_status(resp.state, resp.position, resp.battery);
      } else {
        ESP_LOGW(TAG, "Status from unknown motor %s", mid.c_str());
      }

      // A STATUS report also proves the motor heard us — cancel retransmission.
      // Same hub_id guard as the ACK path (Tuya hub coexistence).
      if (memcmp(resp.hub_id, this->hub_id_, 4) == 0) {
        this->clear_ack_watch_(resp.motor_id);
      }

      // Auto-piggyback: fire queued command when target motor sends status report.
      // Verify hub_id to ensure this is a genuine motor→hub report.
      if (this->piggyback_armed_ &&
          memcmp(resp.motor_id, this->piggyback_motor_, 4) == 0 &&
          memcmp(resp.hub_id, this->hub_id_, 4) == 0) {
        this->piggyback_armed_ = false;
        const char *act_name = action_name_(this->piggyback_action_);
        ESP_LOGI(TAG, "Auto-piggyback FIRED %s on status from %s", act_name, mid.c_str());
        char pbuf[80];
        snprintf(pbuf, sizeof(pbuf), "FIRED %s on %s", act_name, mid.c_str());
        this->piggyback_status_ = pbuf;
        this->send_quick(this->piggyback_motor_, this->piggyback_action_,
                         this->piggyback_position_);
      }
    } else {
      uint8_t extracted[24];
      d492_extract_payload(data.data(), data.size(), extracted, 24);
      char hx[24 * 3 + 1];
      for (int i = 0; i < 24; i++) snprintf(hx + i * 3, 4, "%02x ", extracted[i]);
      char buf[120];
      snprintf(buf, sizeof(buf), "STATUS parse_fail raw=%s", hx);
      this->last_rx_info_ = buf;
    }

  } else if (pkt_len == 20) {
    // Motor pairing response (20 bytes: n=4 encrypted + 4 cleartext)
    this->rx_beacon_count_++;
    SunfreePairingResponse pr{};
    if (parse_pairing_response(data.data(), data.size(), pr)) {
      std::string night = format_motor_id(pr.night_id);
      std::string day = format_motor_id(pr.day_id);
      std::string hid = format_motor_id(pr.hub_id);
      ESP_LOGI(TAG, "PAIRING RESPONSE: night=%s day=%s hub=%s rssi=%.0f",
               night.c_str(), day.c_str(), hid.c_str(), rssi);

      char buf[120];
      snprintf(buf, sizeof(buf), "PAIRED night=%s day=%s hub=%s",
               night.c_str(), day.c_str(), hid.c_str());
      this->last_rx_info_ = buf;

      // Add both motor IDs to discovered list
      auto add_discovered = [this](const std::string &id) {
        for (auto &d : this->discovered_ids_) { if (d == id) return; }
        this->discovered_ids_.push_back(id);
      };
      add_discovered(night);
      add_discovered(day);

      if (this->scan_active_) {
        this->scan_active_ = false;
        char pbuf[120];
        snprintf(pbuf, sizeof(pbuf), "PAIRED! night=%s day=%s", night.c_str(), day.c_str());
        this->pairing_status_ = pbuf;
        ESP_LOGI(TAG, "PAIRING SUCCESS: night=%s day=%s", night.c_str(), day.c_str());
      }
    } else {
      // Log raw for analysis
      uint8_t raw20[20];
      d492_extract_payload(data.data(), data.size(), raw20, 20);
      char hex[20 * 3 + 1];
      for (int i = 0; i < 20; i++) snprintf(hex + i * 3, 4, "%02x ", raw20[i]);
      ESP_LOGW(TAG, "20-byte parse_fail raw: %s rssi=%.0f", hex, rssi);
    }
  }
}

inline void SunfreeHub::send_group_command(const std::string &group,
                                           uint8_t action, uint8_t position) {
  std::vector<SunfreeCover *> targets;
  if (group.empty()) {
    for (auto &pair : this->covers_) targets.push_back(pair.second);
  } else {
    auto it = this->groups_.find(group);
    if (it == this->groups_.end()) {
      ESP_LOGW(TAG, "GROUP '%s' not found", group.c_str());
      return;
    }
    for (auto &mid : it->second) {
      auto cit = this->covers_.find(mid);
      if (cit != this->covers_.end()) targets.push_back(cit->second);
    }
  }
  if (targets.empty()) {
    ESP_LOGW(TAG, "GROUP '%s': no matching covers", group.c_str());
    return;
  }

  std::vector<std::vector<uint8_t>> pkts;
  this->followup_group_entries_.clear();
  for (auto *c : targets) {
    uint8_t seq = this->next_seq();
    uint8_t eff_action = action;
    uint8_t eff_position = position;
    if (c->get_invert_position() && action != ACTION_STOP) {
      if (action == ACTION_OPEN) eff_action = ACTION_CLOSE;
      else if (action == ACTION_CLOSE) eff_action = ACTION_OPEN;
      else if (action == ACTION_POSITION) eff_position = 100 - position;
    }
    pkts.push_back(this->build_command_packet_(c->get_motor_id(), eff_action, eff_position, seq));
    // Watch for this motor's ACK; loop() re-sends unicast if it never confirms.
    this->arm_ack_watch_(c->get_motor_id(), eff_action, eff_position);

    GroupEntry ge{};
    memcpy(ge.motor_id, c->get_motor_id(), 4);
    ge.action = eff_action;
    ge.position = eff_position;
    this->followup_group_entries_.push_back(ge);

    ESP_LOGI(TAG, "GROUP[%s] TX %s seq=0x%02x motor=%s%s",
             group.empty() ? "all" : group.c_str(), action_name_(eff_action),
             seq, c->get_motor_id_str().c_str(),
             c->get_invert_position() ? " (inverted)" : "");
  }
  this->transmit_group_with_preamble_(pkts);

  // Publish what each motor was actually told (the per-motor effective
  // action/position recorded above), not the group-level request — see
  // SunfreeCover::apply_command for why that distinction matters.
  for (size_t i = 0; i < targets.size(); i++) {
    const auto &ge = this->followup_group_entries_[i];
    targets[i]->apply_command(ge.action, ge.position);
  }
}

}  // namespace sunfree_blinds
}  // namespace esphome

#ifdef USE_WEBSERVER_BASE
// Web UI — must come after SunfreeHub and SunfreeCover are fully defined
#include "sunfree_web.h"
#endif
