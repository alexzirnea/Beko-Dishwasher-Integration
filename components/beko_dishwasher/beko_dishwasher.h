#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace beko_dishwasher {

class BekoDishwasher : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_clk_pin(int pin) { clk_pin_ = pin; }
  void set_mosi_pin(int pin) { mosi_pin_ = pin; }

  void set_raw_mosi_sensor(text_sensor::TextSensor *s) { raw_mosi = s; }
  void set_program_sensor(text_sensor::TextSensor *s) { program_name = s; }
  void set_remaining_time_sensor(text_sensor::TextSensor *s) { remaining_time = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { status = s; }
  void set_remaining_minutes_sensor(sensor::Sensor *s) { remaining_minutes = s; }
  void set_power_on_sensor(binary_sensor::BinarySensor *s) { power_on = s; }
  void set_running_sensor(binary_sensor::BinarySensor *s) { running = s; }
  void set_half_load_sensor(binary_sensor::BinarySensor *s) { half_load = s; }
  void set_extra_rinse_sensor(binary_sensor::BinarySensor *s) { extra_rinse = s; }

  // Sensors - just pointers, created in __init__.py
  text_sensor::TextSensor *raw_mosi{nullptr};
  text_sensor::TextSensor *program_name{nullptr};
  text_sensor::TextSensor *remaining_time{nullptr};
  text_sensor::TextSensor *status{nullptr};
  sensor::Sensor *remaining_minutes{nullptr};

  binary_sensor::BinarySensor *power_on{nullptr};
  binary_sensor::BinarySensor *running{nullptr};
  binary_sensor::BinarySensor *half_load{nullptr};
  binary_sensor::BinarySensor *extra_rinse{nullptr};

 private:
  static constexpr size_t MAX_FRAME_BYTES = 32;

  int clk_pin_ = -1;
  int mosi_pin_ = -1;

  uint32_t last_report_ms_ = 0;

  // Written from the ISR, read/cleared from loop() under a short critical section.
  // Arduino's loop() is too slow/jittery (competes with the WiFi stack) to reliably
  // sample a real SPI clock bit-by-bit, so edge capture happens in a GPIO interrupt.
  volatile uint8_t isr_current_byte_ = 0;
  volatile uint8_t isr_bit_count_ = 0;
  volatile uint8_t frame_buf_[MAX_FRAME_BYTES];
  volatile uint8_t frame_len_ = 0;
  volatile uint32_t last_bit_time_us_ = 0;
  volatile uint32_t edge_count_ = 0;

  static void gpio_isr_trampoline(void *arg);
  void on_clk_edge_();

  void process_frame_(const uint8_t *data, size_t len);
  void resync_();

  // Byte 19 is a real per-frame checksum (proven deterministic against ~100 captured
  // frames), but it doesn't match any standard algorithm (XOR/sum/CRC-8/Fletcher, tried
  // exhaustively over every byte range) -- almost certainly a proprietary lookup table in
  // the mainboard firmware, not recoverable from captures alone. The constant framing bytes
  // (0,1,2,18,20,21) serve as the practical validity check instead. Two consecutive failures
  // is treated as bit-sync loss, not just single-bit noise, and forces a resync.
  uint8_t consecutive_failures_ = 0;

  // Every decoded field is a pure function of the raw frame, so comparing the raw bytes
  // against the last *published* frame is enough to skip redundant republishes -- frames
  // arrive ~10/s but real state changes far less often, and without this every sensor would
  // flood Home Assistant's recorder with duplicate states at that rate.
  uint8_t last_published_[MAX_FRAME_BYTES] = {0};
  size_t last_published_len_ = 0;
};

}  // namespace beko_dishwasher
}  // namespace esphome