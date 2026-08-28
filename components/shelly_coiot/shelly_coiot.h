#pragma once

#include <cmath>
#include <vector>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#ifdef USE_ESP_IDF
#include <cerrno>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_netif.h>
#else
#include <WiFiUdp.h>
#endif

namespace esphome {
namespace shelly_coiot {

// CoIoT multicast group and port (Shelly Gen1, CoAP / RFC 7252)
static const char *const COIOT_MCAST_ADDR = "224.0.1.187";
static const uint16_t COIOT_PORT = 5683;

// CoAP option numbers used by CoIoT
static const uint16_t COIOT_OPTION_GLOBAL_DEVID = 3332;
static const uint16_t COIOT_OPTION_STATUS_VALIDITY = 3412;
static const uint16_t COIOT_OPTION_STATUS_SERIAL = 3420;

// CoAP code 0.30 -- the non-standard "publish status" code CoIoT uses
static const uint8_t COIOT_CODE_STATUS = 30;

// Largest CoIoT datagram we are willing to accept. A Shelly 3EM status
// payload is ~600 bytes; 1024 leaves plenty of head room without eating
// the ESP8266 heap.
static const size_t COIOT_MAX_PACKET = 1024;

/// One decoded CoAP message.
struct CoapMessage {
  uint8_t code{0};
  std::string devid;      ///< option 3332, e.g. "SHEM-3#A4CF12345678#2"
  uint16_t validity{0};   ///< option 3412
  uint32_t serial{0};     ///< option 3420
  bool has_serial{false};
  const char *payload{nullptr};
  size_t payload_len{0};
};

#ifdef USE_SENSOR
struct CoiotSensorEntry {
  uint16_t id;
  sensor::Sensor *sensor;
  float factor;
};
#endif

#ifdef USE_BINARY_SENSOR
struct CoiotBinarySensorEntry {
  uint16_t id;
  binary_sensor::BinarySensor *sensor;
};
#endif

class ShellyCoiot : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_mac_filter(const std::string &mac) { this->mac_filter_ = mac; }
  void set_model(const std::string &model) { this->model_ = model; }
  void set_timeout(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }
  void set_mark_unavailable(bool value) { this->mark_unavailable_ = value; }

#ifdef USE_SENSOR
  void register_value_sensor(uint16_t id, sensor::Sensor *s, float factor) {
    this->value_sensors_.push_back(CoiotSensorEntry{id, s, factor});
  }
  void set_total_power_sensor(sensor::Sensor *s) { this->total_power_sensor_ = s; }
  void set_total_energy_sensor(sensor::Sensor *s) { this->total_energy_sensor_ = s; }
  void set_total_energy_returned_sensor(sensor::Sensor *s) { this->total_energy_returned_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void register_bool_sensor(uint16_t id, binary_sensor::BinarySensor *s) {
    this->bool_sensors_.push_back(CoiotBinarySensorEntry{id, s});
  }
  void set_online_sensor(binary_sensor::BinarySensor *s) { this->online_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_ip_text_sensor(text_sensor::TextSensor *s) { this->ip_text_sensor_ = s; }
  void set_device_id_text_sensor(text_sensor::TextSensor *s) { this->device_id_text_sensor_ = s; }
#endif

  /// Last IP the matching Shelly was heard from ("" if never heard).
  std::string get_discovered_ip() const { return this->discovered_ip_; }

 protected:
  bool start_listener_();
  void stop_listener_();
  /// Read at most one datagram. Returns false when the socket is drained.
  bool read_packet_();
  static bool parse_coap_(const uint8_t *buf, size_t len, CoapMessage &out);
  bool devid_matches_(const std::string &devid) const;
  void note_unmatched_(const std::string &devid, const std::string &src_ip);
  void handle_status_(const CoapMessage &msg, const std::string &src_ip);
  void parse_status_payload_(const char *p, size_t len);
  void apply_value_(uint16_t id, float value);
  void publish_totals_();
  void go_offline_();

  std::string mac_filter_;
  std::string model_{"SHEM-3"};
  uint32_t timeout_ms_{60000};
  bool mark_unavailable_{true};

  bool listening_{false};
  bool online_{false};
  uint32_t last_packet_ms_{0};
  uint32_t last_serial_{0xFFFFFFFFu};
  std::string discovered_ip_;
  std::string discovered_devid_;
  bool first_status_logged_{false};
  std::vector<std::string> unmatched_logged_;

  uint8_t buffer_[COIOT_MAX_PACKET];

  // Per-packet accumulators for the derived "total" sensors.
  float phase_power_[3]{NAN, NAN, NAN};
  float phase_energy_[3]{NAN, NAN, NAN};
  float phase_energy_ret_[3]{NAN, NAN, NAN};

#ifdef USE_ESP_IDF
  int socket_fd_{-1};
#else
  WiFiUDP udp_;
#endif

#ifdef USE_SENSOR
  std::vector<CoiotSensorEntry> value_sensors_;
  sensor::Sensor *total_power_sensor_{nullptr};
  sensor::Sensor *total_energy_sensor_{nullptr};
  sensor::Sensor *total_energy_returned_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  std::vector<CoiotBinarySensorEntry> bool_sensors_;
  binary_sensor::BinarySensor *online_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *ip_text_sensor_{nullptr};
  text_sensor::TextSensor *device_id_text_sensor_{nullptr};
#endif
};

}  // namespace shelly_coiot
}  // namespace esphome
