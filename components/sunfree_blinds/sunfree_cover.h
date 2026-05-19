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
    parse_motor_id(id, this->motor_id_);
    this->motor_id_str_ = id;
  }
  void set_battery_sensor(sensor::Sensor *sensor) { this->battery_sensor_ = sensor; }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI - 1; }

  void setup() override {
    this->hub_->register_cover(this);
    ESP_LOGI("sunfree", "Cover '%s' motor_id=%s", this->get_name().c_str(),
             this->motor_id_str_.c_str());
  }

  cover::CoverTraits get_traits() override {
    auto traits = cover::CoverTraits();
    traits.set_supports_position(true);
    traits.set_supports_stop(true);
    traits.set_supports_toggle(false);
    traits.set_is_assumed_state(true);
    return traits;
  }

  void control(const cover::CoverCall &call) override {
    if (call.get_stop()) {
      this->hub_->send_command(this->motor_id_, ACTION_STOP);
      return;
    }
    if (call.get_position().has_value()) {
      float target = *call.get_position();
      float prev = this->position;
      // HA: 0.0=closed, 1.0=open.  Motor: 0=open, 100=closed.
      uint8_t motor_pos = static_cast<uint8_t>((1.0f - target) * 100.0f);
      if (motor_pos == 0) {
        this->hub_->send_command(this->motor_id_, ACTION_OPEN);
      } else if (motor_pos >= 100) {
        this->hub_->send_command(this->motor_id_, ACTION_CLOSE);
      } else {
        this->hub_->send_command(this->motor_id_, ACTION_POSITION, motor_pos);
      }
      this->position = target;
      this->current_operation = (target > prev)
                                    ? cover::COVER_OPERATION_OPENING
                                    : cover::COVER_OPERATION_CLOSING;
      this->publish_state();
    }
  }

  const uint8_t *get_motor_id() const { return this->motor_id_; }
  const std::string &get_motor_id_str() const { return this->motor_id_str_; }
  sensor::Sensor *get_battery_sensor() const { return this->battery_sensor_; }

  void on_ack(const SunfreeResponse &resp) {
    ESP_LOGI("sunfree", "ACK for %s flags=0x%02x", this->motor_id_str_.c_str(), resp.ack_flags);
    if (resp.ack_flags == 0x40) {
      this->current_operation = cover::COVER_OPERATION_IDLE;
    }
  }

  void on_status(uint8_t state, uint8_t motor_position, uint8_t battery) {
    float ha_pos = (100.0f - motor_position) / 100.0f;
    this->position = ha_pos;

    switch (state) {
      case 0:
        this->current_operation = cover::COVER_OPERATION_IDLE;
        break;
      case 1:
        this->current_operation = cover::COVER_OPERATION_CLOSING;
        break;
      case 2:
        this->current_operation = cover::COVER_OPERATION_OPENING;
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
  SunfreeHub *hub_{nullptr};
  uint8_t motor_id_[4]{};
  std::string motor_id_str_;
  sensor::Sensor *battery_sensor_{nullptr};
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
    // Capture raw frame for replay testing
    this->captured_raw_.assign(data.begin(), data.end());
    this->have_capture_ = true;
    // Full hex dump for CRC analysis
    {
      char full[29 * 3 + 1];
      int fn = data.size() < 29 ? data.size() : 29;
      for (int fi = 0; fi < fn; fi++) snprintf(full + fi * 3, 4, "%02x ", data[fi]);
      ESP_LOGI(TAG, "CAPTURED CMD full (%d bytes): %s", fn, full);
    }
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
      this->last_cmd_info_ = buf;
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
  for (auto *c : targets) {
    uint8_t seq = this->next_seq();
    pkts.push_back(this->build_command_packet_(c->get_motor_id(), action, position, seq));
    ESP_LOGI(TAG, "GROUP[%s] TX %s seq=0x%02x motor=%s",
             group.empty() ? "all" : group.c_str(), action_name_(action),
             seq, c->get_motor_id_str().c_str());
  }
  this->transmit_group_with_preamble_(pkts);

  float target_pos = -1;
  if (action == ACTION_OPEN) target_pos = 1.0f;
  else if (action == ACTION_CLOSE) target_pos = 0.0f;
  else if (action == ACTION_POSITION) target_pos = 1.0f - (position / 100.0f);
  for (auto *c : targets) {
    if (action == ACTION_STOP) {
      c->current_operation = cover::COVER_OPERATION_IDLE;
    } else if (target_pos >= 0) {
      float prev = c->position;
      c->position = target_pos;
      c->current_operation = (target_pos > prev)
                                 ? cover::COVER_OPERATION_OPENING
                                 : cover::COVER_OPERATION_CLOSING;
    }
    c->publish_state();
  }
}

}  // namespace sunfree_blinds
}  // namespace esphome

// Web UI — must come after SunfreeHub and SunfreeCover are fully defined
#include "sunfree_web.h"
