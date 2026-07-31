#include "beko_dishwasher.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace beko_dishwasher {

static const char *const TAG = "beko_dishwasher";

void BekoDishwasher::setup() {
  // Pull-down so a floating/unpowered bus idles low instead of drifting on noise;
  // the dishwasher's actively-driven signal easily overrides the weak internal pull.
  pinMode(clk_pin_, INPUT_PULLDOWN);
  pinMode(mosi_pin_, INPUT_PULLDOWN);
  attachInterruptArg(digitalPinToInterrupt(clk_pin_), &BekoDishwasher::gpio_isr_trampoline, this, RISING);
  ESP_LOGI(TAG, "Beko Dishwasher SPI Sniffer started - CLK=%d, MOSI=%d", clk_pin_, mosi_pin_);
}

void IRAM_ATTR BekoDishwasher::gpio_isr_trampoline(void *arg) {
  static_cast<BekoDishwasher *>(arg)->on_clk_edge_();
}

void IRAM_ATTR BekoDishwasher::on_clk_edge_() {
  edge_count_++;
  bool bit = digitalRead(mosi_pin_);
  isr_current_byte_ = (isr_current_byte_ << 1) | (bit ? 1 : 0);
  isr_bit_count_++;
  last_bit_time_us_ = micros();

  if (isr_bit_count_ == 8) {
    if (frame_len_ < MAX_FRAME_BYTES) frame_buf_[frame_len_++] = isr_current_byte_;
    isr_current_byte_ = 0;
    isr_bit_count_ = 0;
  }
}

void BekoDishwasher::resync_() {
  ESP_LOGW(TAG, "Two consecutive frame failures -- resetting bit/byte sync");
  noInterrupts();
  isr_current_byte_ = 0;
  isr_bit_count_ = 0;
  frame_len_ = 0;
  interrupts();
}

void BekoDishwasher::process_frame_(const uint8_t *data, size_t len) {
  std::string raw;
  for (size_t i = 0; i < len; i++) raw += str_sprintf("%02X ", data[i]);
  ESP_LOGD(TAG, "Frame (%zu bytes): %s", len, raw.c_str());

  // The checksum algorithm at byte 19 isn't confirmed yet -- it's a real deterministic
  // per-frame value (proven against ~100 captures) but doesn't match any standard XOR/sum/
  // CRC-8/Fletcher scheme, likely a proprietary lookup table not recoverable from captures
  // alone. The constant framing bytes (0,1,2,18,20,21), always 02 10 2B ... AC ... BD 99 in
  // every genuine frame, serve as the practical validity check instead.
  bool valid = len == 22 && data[0] == 0x02 && data[1] == 0x10 && data[2] == 0x2B &&
               data[18] == 0xAC && data[20] == 0xBD && data[21] == 0x99;

  if (!valid) {
    ESP_LOGD(TAG, "Discarding frame that fails framing check");
    consecutive_failures_++;
    if (consecutive_failures_ >= 2) {
      consecutive_failures_ = 0;
      resync_();
    }
    return;
  }
  consecutive_failures_ = 0;

  if (len == last_published_len_ && memcmp(data, last_published_, len) == 0) {
    candidate_len_ = 0;  // back to the stable state; drop any pending unconfirmed candidate
    return;
  }

  // Content differs from what's currently published -- could be a real change or a
  // one-frame glitch. Only commit it once the same value shows up twice in a row.
  if (len != candidate_len_ || memcmp(data, candidate_, len) != 0) {
    memcpy(candidate_, data, len);
    candidate_len_ = len;
    return;
  }

  memcpy(last_published_, data, len);
  last_published_len_ = len;

  if (raw_mosi) raw_mosi->publish_state(raw);

  // Layout confirmed against logic-analyzer captures (22 bytes, 0-indexed):
  //   0-2   framing            10  beeper (0x64 stop, 0x65 bip-bop, 0x6B bop-bip, 0x69 long beep)
  //   3-5   fixed              11  current program + options bitfield (see below)
  //   6-7   time to finish     12  start/stop LED (0x0E solid, 0x0C flashing, 0x00 off)
  //         (raw only -        13  timer / other
  //          no running-cycle  14  power (0x00 off, 0xFF on)
  //          sample yet to     15  program running (0x00 / 0x01)
  //          calibrate scale)  16-18 fixed/framing, 19 checksum (algorithm not yet confirmed
  //                             from the two captured samples), 20-21 framing
  uint8_t opts = data[11];  // bit0 70C, bit1 Eco, bit2 65C, bit3 60C, bit4 35C, bit5 half load, bit6 extra rinse
                            // (confirmed against the physical panel; the spreadsheet had bit5/bit6 swapped)
  bool is_on = data[14] == 0xFF;
  bool is_running = data[15] == 0x01;

  std::string prog = "Off";
  if (opts & 0x01) prog = "70°C Intensive";
  else if (opts & 0x02) prog = "Eco";
  else if (opts & 0x04) prog = "65°C";
  else if (opts & 0x08) prog = "60°C Quick Shine";
  else if (opts & 0x10) prog = "35°C Mini";
  if (program_name) program_name->publish_state(prog);

  if (half_load) half_load->publish_state(opts & 0x20);
  if (extra_rinse) extra_rinse->publish_state(opts & 0x40);
  if (power_on) power_on->publish_state(is_on);
  if (running) running->publish_state(is_running);

  // Confirmed against a live countdown: byte 6 = hours, byte 7 = minutes, plain binary
  // (not BCD -- caught a 0x11->0x0F rollover with no intervening 0x10, which BCD can't produce).
  if (remaining_time) remaining_time->publish_state(str_sprintf("%d:%02d", data[6], data[7]));
  if (remaining_minutes) remaining_minutes->publish_state(data[6] * 60 + data[7]);

  const char *led = "Unknown";
  if (data[12] == 0x0E) led = "Solid";
  else if (data[12] == 0x0C) led = "Flashing";
  else if (data[12] == 0x00) led = "Off";

  if (status) {
    status->publish_state(str_sprintf("%s | %s | Start/Stop LED %s",
        is_on ? "ON" : "OFF", is_running ? "Running" : "Idle", led));
  }
}

void BekoDishwasher::loop() {
  uint32_t now_ms = millis();

  // Bus has gone idle since the last clock edge the ISR captured -> frame boundary.
  // (No CS line, so a gap in clocking is the only frame delimiter we have.)
  uint8_t local_buf[MAX_FRAME_BYTES];
  uint8_t local_len = 0;

  noInterrupts();
  bool idle = frame_len_ > 0 && isr_bit_count_ == 0 && (micros() - last_bit_time_us_) > 800;
  if (idle) {
    local_len = frame_len_;
    memcpy(local_buf, (const void *) frame_buf_, local_len);
    frame_len_ = 0;
  }
  interrupts();

  if (idle) process_frame_(local_buf, local_len);

  if (now_ms - last_report_ms_ > 2000) {
    noInterrupts();
    uint32_t edges = edge_count_;
    edge_count_ = 0;
    interrupts();
    ESP_LOGD(TAG, "diag: %u CLK rising edges in last %u ms | CLK=%d MOSI=%d",
             edges, now_ms - last_report_ms_, digitalRead(clk_pin_), digitalRead(mosi_pin_));
    last_report_ms_ = now_ms;
  }
}

}  // namespace beko_dishwasher
}  // namespace esphome
