#include "shelly_coiot.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "esphome/core/log.h"

#ifndef USE_ESP_IDF
#ifdef USE_ESP8266
#include <ESP8266WiFi.h>
#else
#include <WiFi.h>
#endif
#endif

namespace esphome {
namespace shelly_coiot {

static const char *const TAG = "shelly_coiot";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static bool is_num_char(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

/// Read a JSON number starting at p[i]. Advances i past the number.
/// Returns false if there is no number there (e.g. `null`), in which case
/// i is advanced past the token anyway.
static bool read_number(const char *p, size_t len, size_t &i, float &out) {
  while (i < len && (p[i] == ' ' || p[i] == '\t')) {
    i++;
  }
  size_t start = i;
  while (i < len && is_num_char(p[i])) {
    i++;
  }
  if (i == start) {
    // not a number (null / true / false) -- skip the token
    while (i < len && p[i] != ',' && p[i] != ']') {
      i++;
    }
    out = NAN;
    return false;
  }
  char tmp[24];
  size_t n = i - start;
  if (n >= sizeof(tmp)) {
    n = sizeof(tmp) - 1;
  }
  memcpy(tmp, p + start, n);
  tmp[n] = '\0';
  out = strtof(tmp, nullptr);
  return true;
}

static void skip_ws(const char *p, size_t len, size_t &i) {
  while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) {
    i++;
  }
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void ShellyCoiot::setup() {
  if (!this->start_listener_()) {
    ESP_LOGW(TAG, "Could not join CoIoT multicast group yet, will retry");
  }
}

bool ShellyCoiot::start_listener_() {
  if (this->listening_) {
    return true;
  }

#ifdef USE_ESP_IDF
  // Find the WiFi station address FIRST. Without it there is nothing to join
  // the multicast group on, and creating/closing a socket on every loop() while
  // WiFi is still associating is wasted work that shows up as a "took a long
  // time for an operation" warning.
  uint32_t sta_addr = 0;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta != nullptr) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
      sta_addr = ip_info.ip.addr;
    }
  }
  if (sta_addr == 0) {
    return false;  // no address yet -- try again on the next loop()
  }

  this->socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->socket_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    return false;
  }

  int reuse = 1;
  ::setsockopt(this->socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(COIOT_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(this->socket_fd_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind() failed: errno %d", errno);
    ::close(this->socket_fd_);
    this->socket_fd_ = -1;
    return false;
  }

  struct ip_mreq mreq {};
  // 224.0.1.187 as a network-order u32 -- avoids depending on the
  // inet_addr() compatibility macro, whose shape varies between
  // lwip build configurations.
  mreq.imr_multiaddr.s_addr = htonl(0xE00001BBu);
  // Join on the WiFi station interface explicitly. Using INADDR_ANY would
  // pick the default route, which is the WireGuard tunnel as soon as the
  // tunnel comes up -- and the Shelly is on the *local* side.
  mreq.imr_interface.s_addr = sta_addr;
  if (::setsockopt(this->socket_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP failed: errno %d", errno);
    ::close(this->socket_fd_);
    this->socket_fd_ = -1;
    return false;
  }

  int flags = ::fcntl(this->socket_fd_, F_GETFL, 0);
  ::fcntl(this->socket_fd_, F_SETFL, flags | O_NONBLOCK);
#else
  // Arduino (ESP8266 / ESP32): WiFiUDP handles the IGMP join for us, but it
  // needs a usable station IP, so bail out until WiFi is actually up.
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress mcast;
  if (!mcast.fromString(COIOT_MCAST_ADDR)) {
    return false;
  }
#ifdef USE_ESP8266
  if (this->udp_.beginMulticast(WiFi.localIP(), mcast, COIOT_PORT) != 1) {
    return false;
  }
#else
  if (this->udp_.beginMulticast(mcast, COIOT_PORT) != 1) {
    return false;
  }
#endif
#endif

  this->listening_ = true;
  ESP_LOGI(TAG, "Listening for CoIoT on %s:%u", COIOT_MCAST_ADDR, COIOT_PORT);
  return true;
}

void ShellyCoiot::stop_listener_() {
  if (!this->listening_) {
    return;
  }
#ifdef USE_ESP_IDF
  if (this->socket_fd_ >= 0) {
    ::close(this->socket_fd_);
    this->socket_fd_ = -1;
  }
#else
  this->udp_.stop();
#endif
  this->listening_ = false;
}

void ShellyCoiot::loop() {
  if (!this->listening_) {
    // Retry the multicast join roughly once a second until it works.
    static uint32_t last_try = 0;
    if (millis() - last_try > 1000) {
      last_try = millis();
      this->start_listener_();
    }
    return;
  }

  // Drain whatever is queued, but never more than a handful per loop() so we
  // do not starve the rest of the firmware.
  for (uint8_t i = 0; i < 4; i++) {
    if (!this->read_packet_()) {
      break;
    }
  }

  if (this->online_ && this->timeout_ms_ > 0 && (millis() - this->last_packet_ms_) > this->timeout_ms_) {
    this->go_offline_();
  }
}

bool ShellyCoiot::read_packet_() {
  size_t len = 0;
  std::string src_ip;

#ifdef USE_ESP_IDF
  struct sockaddr_in from {};
  socklen_t from_len = sizeof(from);
  int n = ::recvfrom(this->socket_fd_, this->buffer_, sizeof(this->buffer_), 0, (struct sockaddr *) &from, &from_len);
  if (n <= 0) {
    return false;
  }
  len = (size_t) n;
  // Format the source address by hand: lwip's inet_ntoa_r is a macro whose
  // signature varies between IDF versions.
  uint32_t a = ntohl(from.sin_addr.s_addr);
  char ipbuf[16];
  snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", (unsigned) ((a >> 24) & 0xFF), (unsigned) ((a >> 16) & 0xFF),
           (unsigned) ((a >> 8) & 0xFF), (unsigned) (a & 0xFF));
  src_ip = ipbuf;
#else
  int packet_size = this->udp_.parsePacket();
  if (packet_size <= 0) {
    return false;
  }
  if ((size_t) packet_size > sizeof(this->buffer_)) {
    ESP_LOGW(TAG, "CoIoT packet too large (%d bytes), dropped", packet_size);
    this->udp_.flush();
    return true;
  }
  int n = this->udp_.read(this->buffer_, sizeof(this->buffer_));
  if (n <= 0) {
    return true;
  }
  len = (size_t) n;
  src_ip = this->udp_.remoteIP().toString().c_str();
#endif

  CoapMessage msg;
  if (!parse_coap_(this->buffer_, len, msg)) {
    return true;  // malformed -- ignore, but keep draining
  }
  if (msg.code != COIOT_CODE_STATUS || msg.payload == nullptr || msg.payload_len == 0) {
    return true;  // description packet or something else, not a status publish
  }
  if (!this->devid_matches_(msg.devid)) {
    this->note_unmatched_(msg.devid, src_ip);
    return true;
  }

  this->handle_status_(msg, src_ip);
  return true;
}

// ---------------------------------------------------------------------------
// CoAP decoding
// ---------------------------------------------------------------------------

bool ShellyCoiot::parse_coap_(const uint8_t *buf, size_t len, CoapMessage &out) {
  if (len < 4) {
    return false;
  }
  uint8_t version = (buf[0] >> 6) & 0x03;
  if (version != 1) {
    return false;
  }
  uint8_t tkl = buf[0] & 0x0F;
  if (tkl > 8) {
    return false;
  }
  out.code = buf[1];

  size_t pos = 4 + tkl;
  if (pos > len) {
    return false;
  }

  uint16_t option_number = 0;
  while (pos < len) {
    uint8_t b = buf[pos];
    if (b == 0xFF) {  // payload marker
      pos++;
      out.payload = (const char *) (buf + pos);
      out.payload_len = len - pos;
      return true;
    }
    uint8_t delta_nib = b >> 4;
    uint8_t len_nib = b & 0x0F;
    pos++;

    uint16_t delta;
    if (delta_nib == 13) {
      if (pos >= len)
        return false;
      delta = (uint16_t) buf[pos] + 13;
      pos += 1;
    } else if (delta_nib == 14) {
      if (pos + 1 >= len)
        return false;
      delta = (uint16_t) ((buf[pos] << 8) | buf[pos + 1]) + 269;
      pos += 2;
    } else if (delta_nib == 15) {
      return false;
    } else {
      delta = delta_nib;
    }

    uint16_t opt_len;
    if (len_nib == 13) {
      if (pos >= len)
        return false;
      opt_len = (uint16_t) buf[pos] + 13;
      pos += 1;
    } else if (len_nib == 14) {
      if (pos + 1 >= len)
        return false;
      opt_len = (uint16_t) ((buf[pos] << 8) | buf[pos + 1]) + 269;
      pos += 2;
    } else if (len_nib == 15) {
      return false;
    } else {
      opt_len = len_nib;
    }

    option_number += delta;
    if (pos + opt_len > len) {
      return false;
    }

    switch (option_number) {
      case COIOT_OPTION_GLOBAL_DEVID:
        out.devid.assign((const char *) (buf + pos), opt_len);
        break;
      case COIOT_OPTION_STATUS_VALIDITY:
        out.validity = 0;
        for (uint16_t k = 0; k < opt_len && k < 2; k++) {
          out.validity = (uint16_t) ((out.validity << 8) | buf[pos + k]);
        }
        break;
      case COIOT_OPTION_STATUS_SERIAL:
        out.serial = 0;
        for (uint16_t k = 0; k < opt_len && k < 4; k++) {
          out.serial = (out.serial << 8) | buf[pos + k];
        }
        out.has_serial = true;
        break;
      default:
        break;
    }
    pos += opt_len;
  }

  out.payload = nullptr;
  out.payload_len = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Discovery / filtering
// ---------------------------------------------------------------------------

bool ShellyCoiot::devid_matches_(const std::string &devid) const {
  if (devid.empty()) {
    return false;
  }
  // devid looks like "SHEM-3#A4CF12345678#2"
  if (!this->model_.empty()) {
    if (devid.compare(0, this->model_.size(), this->model_) != 0) {
      return false;
    }
    if (devid.size() <= this->model_.size() || devid[this->model_.size()] != '#') {
      return false;
    }
  }
  if (this->mac_filter_.empty()) {
    return true;
  }
  // Compare the MAC field case-insensitively.
  size_t first = devid.find('#');
  if (first == std::string::npos) {
    return false;
  }
  size_t second = devid.find('#', first + 1);
  std::string mac = (second == std::string::npos) ? devid.substr(first + 1) : devid.substr(first + 1, second - first - 1);
  if (mac.size() != this->mac_filter_.size()) {
    return false;
  }
  for (size_t i = 0; i < mac.size(); i++) {
    char a = mac[i];
    if (a >= 'a' && a <= 'z') {
      a = (char) (a - 'a' + 'A');
    }
    if (a != this->mac_filter_[i]) {
      return false;
    }
  }
  return true;
}

/// Report a Shelly we can hear but are configured to ignore -- once per device.
/// This is what tells you the MAC to put into `mac:` when you do not know it,
/// and what explains the silence when the filter is wrong.
void ShellyCoiot::note_unmatched_(const std::string &devid, const std::string &src_ip) {
  if (devid.empty()) {
    return;
  }
  for (const auto &seen : this->unmatched_logged_) {
    if (seen == devid) {
      return;
    }
  }
  if (this->unmatched_logged_.size() >= 8) {
    return;  // do not let a busy network grow this without bound
  }
  this->unmatched_logged_.push_back(devid);
  ESP_LOGI(TAG, "Heard CoIoT device '%s' at %s, but it does not match the filter (model '%s', mac '%s')",
           devid.c_str(), src_ip.c_str(), this->model_.empty() ? "(any)" : this->model_.c_str(),
           this->mac_filter_.empty() ? "(any)" : this->mac_filter_.c_str());
}

// ---------------------------------------------------------------------------
// Status handling
// ---------------------------------------------------------------------------

void ShellyCoiot::handle_status_(const CoapMessage &msg, const std::string &src_ip) {
  this->last_packet_ms_ = millis();

  if (src_ip != this->discovered_ip_) {
    ESP_LOGI(TAG, "Shelly %s discovered at %s", msg.devid.c_str(), src_ip.c_str());
    this->discovered_ip_ = src_ip;
#ifdef USE_TEXT_SENSOR
    if (this->ip_text_sensor_ != nullptr) {
      this->ip_text_sensor_->publish_state(src_ip);
    }
#endif
  }
  if (msg.devid != this->discovered_devid_) {
    this->discovered_devid_ = msg.devid;
#ifdef USE_TEXT_SENSOR
    if (this->device_id_text_sensor_ != nullptr) {
      this->device_id_text_sensor_->publish_state(msg.devid);
    }
#endif
  }

  if (!this->online_) {
    this->online_ = true;
#ifdef USE_BINARY_SENSOR
    if (this->online_sensor_ != nullptr) {
      this->online_sensor_->publish_state(true);
    }
#endif
  }

  // The status serial only changes when something in the report changed.
  // Skipping repeats saves a good chunk of CPU on an ESP8266.
  if (msg.has_serial && msg.serial == this->last_serial_) {
    ESP_LOGVV(TAG, "Duplicate status serial %u, skipped", msg.serial);
    return;
  }
  if (msg.has_serial) {
    this->last_serial_ = msg.serial;
  }

  this->parse_status_payload_(msg.payload, msg.payload_len);
  this->publish_totals_();
}

void ShellyCoiot::parse_status_payload_(const char *p, size_t len) {
  // Expected shape: {"G":[[0,4105,123.45],[0,4106,678],...]}
  size_t i = 0;
  bool found_g = false;
  for (size_t k = 0; k + 2 < len; k++) {
    if (p[k] == '"' && p[k + 1] == 'G' && p[k + 2] == '"') {
      i = k + 3;
      found_g = true;
      break;
    }
  }
  if (!found_g) {
    ESP_LOGW(TAG, "CoIoT payload without \"G\" array");
    return;
  }

  skip_ws(p, len, i);
  if (i >= len || p[i] != ':') {
    return;
  }
  i++;
  skip_ws(p, len, i);
  if (i >= len || p[i] != '[') {
    return;
  }
  i++;  // now inside the outer array

  uint8_t count = 0;
  while (i < len) {
    skip_ws(p, len, i);
    if (i < len && p[i] == ',') {
      i++;
      skip_ws(p, len, i);
    }
    if (i >= len || p[i] == ']') {
      break;
    }
    if (p[i] != '[') {
      break;  // unexpected -- stop rather than loop forever
    }
    i++;

    float channel = NAN, id_f = NAN, value = NAN;
    read_number(p, len, i, channel);
    skip_ws(p, len, i);
    if (i >= len || p[i] != ',')
      break;
    i++;
    bool have_id = read_number(p, len, i, id_f);
    skip_ws(p, len, i);
    if (i >= len || p[i] != ',')
      break;
    i++;
    bool have_value = read_number(p, len, i, value);
    skip_ws(p, len, i);
    // tolerate extra fields in the tuple
    while (i < len && p[i] != ']') {
      i++;
    }
    if (i < len && p[i] == ']') {
      i++;
    }

    if (have_id && have_value) {
      this->apply_value_((uint16_t) id_f, value);
      count++;
    }
  }
  if (!this->first_status_logged_) {
    this->first_status_logged_ = true;
    ESP_LOGI(TAG, "First CoIoT status decoded: %u values from %s", count, this->discovered_ip_.c_str());
  } else {
    ESP_LOGD(TAG, "CoIoT status from %s: %u values", this->discovered_ip_.c_str(), count);
  }
}

void ShellyCoiot::apply_value_(uint16_t id, float value) {
  // Cache the per-phase values we need for the derived totals.
  // 4105/4205/4305 = power, 4106/... = energy, 4107/... = energy returned
  if (id >= 4105 && id <= 4307) {
    uint16_t rel = id - 4100;
    uint16_t ch = rel / 100;
    uint16_t off = rel % 100;
    if (ch < 3) {
      if (off == 5) {
        this->phase_power_[ch] = value;
      } else if (off == 6) {
        this->phase_energy_[ch] = value;
      } else if (off == 7) {
        this->phase_energy_ret_[ch] = value;
      }
    }
  }

#ifdef USE_SENSOR
  for (auto &entry : this->value_sensors_) {
    if (entry.id == id) {
      entry.sensor->publish_state(value * entry.factor);
    }
  }
#endif
#ifdef USE_BINARY_SENSOR
  for (auto &entry : this->bool_sensors_) {
    if (entry.id == id) {
      entry.sensor->publish_state(value != 0.0f);
    }
  }
#endif
}

void ShellyCoiot::publish_totals_() {
#ifdef USE_SENSOR
  if (this->total_power_sensor_ != nullptr && !std::isnan(this->phase_power_[0]) &&
      !std::isnan(this->phase_power_[1]) && !std::isnan(this->phase_power_[2])) {
    this->total_power_sensor_->publish_state(this->phase_power_[0] + this->phase_power_[1] + this->phase_power_[2]);
  }
  if (this->total_energy_sensor_ != nullptr && !std::isnan(this->phase_energy_[0]) &&
      !std::isnan(this->phase_energy_[1]) && !std::isnan(this->phase_energy_[2])) {
    float wh = this->phase_energy_[0] + this->phase_energy_[1] + this->phase_energy_[2];
    this->total_energy_sensor_->publish_state(wh / 1000.0f);
  }
  if (this->total_energy_returned_sensor_ != nullptr && !std::isnan(this->phase_energy_ret_[0]) &&
      !std::isnan(this->phase_energy_ret_[1]) && !std::isnan(this->phase_energy_ret_[2])) {
    float wh = this->phase_energy_ret_[0] + this->phase_energy_ret_[1] + this->phase_energy_ret_[2];
    this->total_energy_returned_sensor_->publish_state(wh / 1000.0f);
  }
#endif
}

void ShellyCoiot::go_offline_() {
  ESP_LOGW(TAG, "No CoIoT packet for %u ms -- Shelly considered offline", this->timeout_ms_);
  this->online_ = false;
  this->last_serial_ = 0xFFFFFFFFu;

#ifdef USE_BINARY_SENSOR
  if (this->online_sensor_ != nullptr) {
    this->online_sensor_->publish_state(false);
  }
#endif

  if (!this->mark_unavailable_) {
    return;
  }
#ifdef USE_SENSOR
  for (auto &entry : this->value_sensors_) {
    entry.sensor->publish_state(NAN);
  }
  if (this->total_power_sensor_ != nullptr) {
    this->total_power_sensor_->publish_state(NAN);
  }
  if (this->total_energy_sensor_ != nullptr) {
    this->total_energy_sensor_->publish_state(NAN);
  }
  if (this->total_energy_returned_sensor_ != nullptr) {
    this->total_energy_returned_sensor_->publish_state(NAN);
  }
#endif
  for (uint8_t i = 0; i < 3; i++) {
    this->phase_power_[i] = NAN;
    this->phase_energy_[i] = NAN;
    this->phase_energy_ret_[i] = NAN;
  }
}

void ShellyCoiot::dump_config() {
  ESP_LOGCONFIG(TAG, "Shelly CoIoT listener:");
  ESP_LOGCONFIG(TAG, "  Multicast group: %s:%u", COIOT_MCAST_ADDR, COIOT_PORT);
  ESP_LOGCONFIG(TAG, "  Model filter: %s", this->model_.empty() ? "(any)" : this->model_.c_str());
  ESP_LOGCONFIG(TAG, "  MAC filter: %s", this->mac_filter_.empty() ? "(any)" : this->mac_filter_.c_str());
  ESP_LOGCONFIG(TAG, "  Timeout: %u ms", this->timeout_ms_);
  if (!this->discovered_ip_.empty()) {
    ESP_LOGCONFIG(TAG, "  Discovered at: %s (%s)", this->discovered_ip_.c_str(), this->discovered_devid_.c_str());
  }
}

}  // namespace shelly_coiot
}  // namespace esphome
