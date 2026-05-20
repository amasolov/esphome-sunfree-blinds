#pragma once
#ifdef USE_WEBSERVER_BASE
// Included from bottom of sunfree_cover.h — all types must be complete.
#include "esphome/components/web_server_base/web_server_base.h"

namespace esphome {
namespace sunfree_blinds {

static const char SUNFREE_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sunfree Blinds</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:12px;max-width:600px;margin:0 auto}
h1{font-size:1.3em;margin-bottom:8px;color:#64b5f6}
.hub{background:#16213e;padding:10px 14px;border-radius:8px;margin-bottom:12px;font-size:.85em}
.hub span{color:#90caf9;font-family:monospace}
.card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:10px}
.card h2{font-size:1em;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center}
.card h2 small{font-size:.75em;color:#888;font-weight:normal;font-family:monospace}
.row{display:flex;gap:6px;margin-bottom:8px;flex-wrap:wrap}
.btn{border:none;border-radius:6px;padding:7px 12px;font-size:.8em;cursor:pointer;color:#fff;flex:1;min-width:60px;text-align:center}
.btn:active{opacity:.7}
.b-open{background:#2e7d32}.b-close{background:#c62828}.b-stop{background:#f57f17}
.b-cfg{background:#37474f}.b-fav{background:#6a1b9a}
.b-pair{background:#1565c0}.b-pair.active{background:#e65100}
.pos{display:flex;align-items:center;gap:8px;margin-bottom:6px;font-size:.85em}
.pos input{flex:1;accent-color:#64b5f6}
.pos span{min-width:36px;text-align:right}
.info{font-size:.75em;color:#999;margin-top:4px}
.pair-box{background:#16213e;border-radius:8px;padding:14px;margin-bottom:10px}
.pair-box h2{font-size:1em;margin-bottom:8px;color:#ffab40}
.status{color:#81c784;font-family:monospace;font-size:.85em}
.disc{margin-top:8px;font-size:.85em}
.disc code{background:#0d1b2a;padding:2px 8px;border-radius:4px;font-size:1em;user-select:all;color:#ffab40}
</style></head><body>
<h1>Sunfree Blinds</h1>
<div class="hub">Hub ID: <span id="hid">...</span></div>
<div class="pair-box">
<h2>Pairing</h2>
<div class="row"><button class="btn b-pair" onclick="pair('start')">Start Scan</button>
<button class="btn b-pair active" onclick="pair('stop')">Stop Scan</button></div>
<div class="status" id="pstatus">...</div>
<div id="discovered"></div>
</div>
<div id="motors"></div>
<script>
const B='/sunfree';
function post(p){return fetch(B+'/cmd?'+p,{method:'POST'}).then(r=>r.json())}
function pair(a){post('action=pair_'+a)}
function cmd(m,a,v){let q='motor='+m+'&action='+a;if(v!==undefined)q+='&value='+v;post(q)}
function grp(g,a,v){let q='action=group_'+a+'&group='+encodeURIComponent(g);if(v!==undefined)q+='&value='+v;post(q)}
function setPos(m,el){cmd(m,'position',el.value)}
function grpPos(g,el){grp(g,'position',el.value)}
function render(d){
  document.getElementById('hid').textContent=d.hub_id;
  document.getElementById('pstatus').textContent=d.pairing;
  let dd=document.getElementById('discovered');
  if(d.discovered&&d.discovered.length){
    let dh='<div class="disc"><b>Discovered motor IDs</b> (add to YAML):<br>';
    d.discovered.forEach(id=>{dh+='<code>'+id+'</code> '});
    dh+='</div>';dd.innerHTML=dh;
  } else {dd.innerHTML='';}
  const c=document.getElementById('motors');
  if(!d.motors||!d.motors.length){c.innerHTML='<div class="card"><em>No motors configured</em></div>';return}
  let h='';
  function groupCard(name,label){
    h+='<div class="card" style="border:1px solid #64b5f6"><h2 style="color:#64b5f6">'+label+'</h2>';
    h+='<div class="pos"><span></span><input type="range" min="0" max="100" value="0" onchange="grpPos(\''+name+'\',this)"><span></span></div>';
    h+='<div class="row">';
    h+='<button class="btn b-open" onclick="grp(\''+name+'\',\'open\')">Open</button>';
    h+='<button class="btn b-stop" onclick="grp(\''+name+'\',\'stop\')">Stop</button>';
    h+='<button class="btn b-close" onclick="grp(\''+name+'\',\'close\')">Close</button>';
    h+='</div></div>';
  }
  if(d.groups&&d.groups.length){d.groups.forEach(g=>groupCard(g.name,g.name+' ('+g.motor_ids.length+')'));}
  groupCard('','All Motors ('+d.motors.length+')');
  d.motors.forEach(m=>{
    let pct=Math.round((1-m.position)*100);
    let op=m.operation==='OPENING'?'Opening':m.operation==='CLOSING'?'Closing':'Idle';
    let bat=m.battery>=0?m.battery+'%':'?';
    h+='<div class="card"><h2>'+m.name+' <small>'+m.id+'</small></h2>';
    h+='<div class="pos"><span>'+pct+'%</span>';
    h+='<input type="range" min="0" max="100" value="'+pct+'" onchange="setPos(\''+m.id+'\',this)">';
    h+='<span>Bat:'+bat+'</span></div>';
    h+='<div class="info">'+op+'</div>';
    h+='<div class="row">';
    h+='<button class="btn b-open" onclick="cmd(\''+m.id+'\',\'open\')">Open</button>';
    h+='<button class="btn b-stop" onclick="cmd(\''+m.id+'\',\'stop\')">Stop</button>';
    h+='<button class="btn b-close" onclick="cmd(\''+m.id+'\',\'close\')">Close</button>';
    h+='</div>';
    h+='<div class="row">';
    h+='<button class="btn b-fav" onclick="cmd(\''+m.id+'\',\'goto_favourite\')">Go Fav</button>';
    h+='<button class="btn b-cfg" onclick="cmd(\''+m.id+'\',\'save_favourite\')">Save Fav</button>';
    h+='</div>';
    h+='<div class="row">';
    h+='<button class="btn b-cfg" onclick="cmd(\''+m.id+'\',\'direction_forward\')">Dir Fwd</button>';
    h+='<button class="btn b-cfg" onclick="cmd(\''+m.id+'\',\'direction_reverse\')">Dir Rev</button>';
    h+='</div>';
    h+='<div class="row">';
    h+='<button class="btn b-cfg" onclick="cmd(\''+m.id+'\',\'set_open_limit\')">Set Open Lim</button>';
    h+='<button class="btn b-cfg" onclick="cmd(\''+m.id+'\',\'set_close_limit\')">Set Close Lim</button>';
    h+='</div></div>';
  });
  c.innerHTML=h;
}
function poll(){fetch(B+'/status').then(r=>r.json()).then(render).catch(()=>{})}
poll();setInterval(poll,3000);
</script></body></html>
)rawhtml";

class SunfreeWebHandler : public AsyncWebHandler {
 public:
  SunfreeWebHandler(SunfreeHub *hub) : hub_(hub) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    auto url = request->url_to(buf);
    return url.find("/sunfree") == 0;
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    std::string url(request->url_to(url_buf));

    if (url == "/sunfree" || url == "/sunfree/") {
      request->send(200, "text/html", SUNFREE_HTML);
      return;
    }

    if (url == "/sunfree/status") {
      std::string json = this->hub_->get_motors_json();
      request->send(200, "application/json", json.c_str());
      return;
    }

    if (url == "/sunfree/cmd") {
      std::string action = this->get_arg_(request, "action");
      std::string motor = this->get_arg_(request, "motor");

      if (action == "pair_start") {
        this->hub_->start_scan();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }
      if (action == "pair_stop") {
        this->hub_->stop_scan();
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }

      if (action.substr(0, 6) == "group_") {
        std::string grp_action = action.substr(6);
        std::string group = this->get_arg_(request, "group");
        uint8_t act, pos = 0;
        if (grp_action == "open") act = ACTION_OPEN;
        else if (grp_action == "close") act = ACTION_CLOSE;
        else if (grp_action == "stop") act = ACTION_STOP;
        else if (grp_action == "position") {
          act = ACTION_POSITION;
          std::string val = this->get_arg_(request, "value");
          pos = val.empty() ? 0 : static_cast<uint8_t>(std::atoi(val.c_str()));
        } else {
          request->send(400, "application/json", "{\"error\":\"unknown group action\"}");
          return;
        }
        this->hub_->send_group_command(group, act, pos);
        request->send(200, "application/json", "{\"ok\":true}");
        return;
      }

      if (motor.empty()) {
        request->send(400, "application/json", "{\"error\":\"missing motor\"}");
        return;
      }

      uint8_t mid[4];
      parse_motor_id(motor, mid);

      if (action == "open") {
        this->hub_->send_command(mid, ACTION_OPEN);
      } else if (action == "close") {
        this->hub_->send_command(mid, ACTION_CLOSE);
      } else if (action == "stop") {
        this->hub_->send_command(mid, ACTION_STOP);
      } else if (action == "position") {
        std::string val = this->get_arg_(request, "value");
        uint8_t pos = val.empty() ? 0 : static_cast<uint8_t>(std::atoi(val.c_str()));
        this->hub_->send_command(mid, ACTION_POSITION, pos);
      } else if (action == "direction_forward") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION), DIR_FORWARD);
      } else if (action == "direction_reverse") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::SET_DIRECTION), DIR_REVERSE);
      } else if (action == "set_open_limit") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::SET_LIMIT), LIMIT_OPEN);
      } else if (action == "set_close_limit") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::SET_LIMIT), LIMIT_CLOSE);
      } else if (action == "save_favourite") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::SET_LIMIT), LIMIT_FAVOURITE);
      } else if (action == "goto_favourite") {
        this->hub_->send_config(mid, static_cast<uint8_t>(SunfreeCmd::GOTO_FAVOURITE), GOTO_FAV_VALUE);
      } else {
        request->send(400, "application/json", "{\"error\":\"unknown action\"}");
        return;
      }

      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }

    request->send(404, "text/plain", "Not found");
  }

 private:
  SunfreeHub *hub_;

  static std::string get_arg_(AsyncWebServerRequest *request, const char *name) {
    if (request->hasArg(name))
      return std::string(request->arg(name).c_str());
    return "";
  }
};

// Implemented here because SunfreeHub + SunfreeCover are fully defined.
inline void SunfreeHub::setup_web_() {
  auto *handler = new SunfreeWebHandler(this);
  this->web_base_->add_handler(handler);
  ESP_LOGI(TAG, "Web UI registered at /sunfree");
}

inline std::string SunfreeHub::get_motors_json() {
  std::string json = "{\"hub_id\":\"" + format_motor_id(this->hub_id_) + "\",";
  json += "\"pairing\":\"" + this->pairing_status_ + "\",";
  json += "\"discovered\":[";
  for (size_t i = 0; i < this->discovered_ids_.size(); i++) {
    if (i) json += ",";
    json += "\"" + this->discovered_ids_[i] + "\"";
  }
  json += "],";
  json += "\"groups\":[";
  bool gfirst = true;
  for (auto &grp : this->groups_) {
    if (!gfirst) json += ",";
    gfirst = false;
    json += "{\"name\":\"" + grp.first + "\",\"motor_ids\":[";
    bool mfirst = true;
    for (auto &mid : grp.second) {
      if (!mfirst) json += ",";
      mfirst = false;
      json += "\"" + mid + "\"";
    }
    json += "]}";
  }
  json += "],\"motors\":[";
  bool first = true;
  for (auto &pair : this->covers_) {
    if (!first) json += ",";
    first = false;
    SunfreeCover *c = pair.second;
    const char *op = "IDLE";
    if (c->current_operation == cover::COVER_OPERATION_OPENING) op = "OPENING";
    else if (c->current_operation == cover::COVER_OPERATION_CLOSING) op = "CLOSING";
    float bat = -1;
    if (c->get_battery_sensor() && c->get_battery_sensor()->has_state())
      bat = c->get_battery_sensor()->state;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"name\":\"%s\",\"id\":\"%s\",\"position\":%.3f,\"operation\":\"%s\",\"battery\":%.0f}",
             c->get_name().c_str(), pair.first.c_str(), c->position, op, bat);
    json += buf;
  }
  json += "]}";
  return json;
}

}  // namespace sunfree_blinds
}  // namespace esphome
#endif  // USE_WEBSERVER_BASE
