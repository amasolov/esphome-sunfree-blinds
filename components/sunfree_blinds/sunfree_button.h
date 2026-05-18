#pragma once
#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "sunfree_hub.h"
#include "sunfree_protocol.h"

namespace esphome {
namespace sunfree_blinds {

enum SunfreeButtonAction : uint8_t {
  BTN_DIRECTION_FORWARD = 0,
  BTN_DIRECTION_REVERSE = 1,
  BTN_SET_OPEN_LIMIT = 2,
  BTN_SET_CLOSE_LIMIT = 3,
  BTN_SAVE_FAVOURITE = 4,
  BTN_GOTO_FAVOURITE = 5,
};

class SunfreeButton : public button::Button, public Component {
 public:
  void set_hub(SunfreeHub *hub) { this->hub_ = hub; }
  void set_motor_id(const std::string &id) {
    parse_motor_id(id, this->motor_id_);
    this->motor_id_str_ = id;
  }
  void set_action_type(uint8_t type) { this->action_type_ = static_cast<SunfreeButtonAction>(type); }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI - 2; }

  void press_action() override {
    uint8_t action, value;
    switch (this->action_type_) {
      case BTN_DIRECTION_FORWARD:
        action = static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION);
        value = DIR_FORWARD;
        break;
      case BTN_DIRECTION_REVERSE:
        action = static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION);
        value = DIR_REVERSE;
        break;
      case BTN_SET_OPEN_LIMIT:
        action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
        value = LIMIT_OPEN;
        break;
      case BTN_SET_CLOSE_LIMIT:
        action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
        value = LIMIT_CLOSE;
        break;
      case BTN_SAVE_FAVOURITE:
        action = static_cast<uint8_t>(SunfreeCmd::SET_LIMIT);
        value = LIMIT_FAVOURITE;
        break;
      case BTN_GOTO_FAVOURITE:
        action = static_cast<uint8_t>(SunfreeCmd::GOTO_FAVOURITE);
        value = GOTO_FAV_VALUE;
        break;
      default:
        ESP_LOGW("sunfree_btn", "Unknown button action type %d", this->action_type_);
        return;
    }
    ESP_LOGI("sunfree_btn", "Button '%s' pressed: action=0x%02X value=0x%02X motor=%s",
             this->get_name().c_str(), action, value, this->motor_id_str_.c_str());
    this->hub_->send_config(this->motor_id_, action, value);
  }

 protected:
  SunfreeHub *hub_{nullptr};
  uint8_t motor_id_[4]{};
  std::string motor_id_str_;
  SunfreeButtonAction action_type_{BTN_DIRECTION_FORWARD};
};

}  // namespace sunfree_blinds
}  // namespace esphome
