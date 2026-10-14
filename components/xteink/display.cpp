#include "display.h"

#include "esphome/core/log.h"

#include <cstdlib>
#include <cstring>
#include <esp_heap_caps.h>

namespace esphome {
namespace xteink {

static const char *const TAG = "xteink.display";

void XteinkDisplay::setup() {
  // The FreeInk refresh calls used below are synchronous, and this component has
  // no loop work. ESPHome's LVGL update_when_display_idle gate checks the base
  // Component lifecycle state, so leaving our empty loop enabled makes the
  // display appear permanently busy and prevents LVGL from rendering at all.
  this->disable_loop();

  if (!this->grayscale_enabled_)
    return;

  auto &epd = this->parent_->display();
  const auto capabilities = epd.grayscaleCapabilities(freeink::GrayscaleMode::Absolute);
  if (!capabilities.supported()) {
    ESP_LOGW(TAG, "The detected panel does not support absolute four-level grayscale; using monochrome");
    this->grayscale_warning_logged_ = true;
    return;
  }

  this->gray_buffer_size_ = static_cast<size_t>(epd.getDisplayWidthBytes()) * epd.getDisplayHeight();
  this->gray_luma_size_ = static_cast<size_t>(epd.getDisplayWidth()) * epd.getDisplayHeight();
#if defined(BOARD_HAS_PSRAM)
  this->gray_luma_ =
      static_cast<uint8_t *>(heap_caps_malloc(this->gray_luma_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->gray_lsb_ =
      static_cast<uint8_t *>(heap_caps_malloc(this->gray_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  this->gray_msb_ =
      static_cast<uint8_t *>(heap_caps_malloc(this->gray_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#endif
  if (this->gray_luma_ == nullptr)
    this->gray_luma_ = static_cast<uint8_t *>(malloc(this->gray_luma_size_));
  if (this->gray_lsb_ == nullptr)
    this->gray_lsb_ = static_cast<uint8_t *>(malloc(this->gray_buffer_size_));
  if (this->gray_msb_ == nullptr)
    this->gray_msb_ = static_cast<uint8_t *>(malloc(this->gray_buffer_size_));
  if (this->gray_luma_ == nullptr || this->gray_lsb_ == nullptr || this->gray_msb_ == nullptr) {
    free(this->gray_luma_);
    free(this->gray_lsb_);
    free(this->gray_msb_);
    this->gray_luma_ = nullptr;
    this->gray_lsb_ = nullptr;
    this->gray_msb_ = nullptr;
    ESP_LOGW(TAG, "Could not allocate %u bytes for continuous-tone grayscale; using monochrome",
             static_cast<unsigned>(this->gray_luma_size_ + this->gray_buffer_size_ * 2));
    this->grayscale_warning_logged_ = true;
    return;
  }
  memset(this->gray_luma_, 0xFF, this->gray_luma_size_);
  memset(this->gray_lsb_, 0xFF, this->gray_buffer_size_);
  memset(this->gray_msb_, 0xFF, this->gray_buffer_size_);
  this->grayscale_available_ = true;
  ESP_LOGI(TAG, "Continuous-tone grayscale enabled (%u bytes)",
           static_cast<unsigned>(this->gray_luma_size_ + this->gray_buffer_size_ * 2));
}

void XteinkDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  auto &epd = this->parent_->display();
  if (x < 0 || y < 0 || x >= epd.getDisplayWidth() || y >= epd.getDisplayHeight())
    return;
  uint8_t *buf = epd.getFrameBuffer();
  const uint32_t idx = y * epd.getDisplayWidthBytes() + (x >> 3);
  const uint8_t bit = 0x80 >> (x & 7);

  // ESPHome's native binary drawing API uses COLOR_ON for marked (black) pixels,
  // while draw_pixels_at() receives literal RGB from LVGL where black is 0 and
  // white is 255. Keep both contracts instead of applying Color::is_on() to RGB.
  const uint16_t luma = (77U * color.r + 150U * color.g + 29U * color.b + 128U) >> 8;
  const bool black = this->rgb_input_ ? luma < 128 : color.is_on();
  const uint8_t old = buf[idx];
  if (black) {
    buf[idx] &= static_cast<uint8_t>(~bit);  // 0 = black
  } else {
    buf[idx] |= bit;  // 1 = white
  }
  this->buffer_dirty_ |= old != buf[idx];

  if (this->grayscale_available_) {
    // Preserve 8-bit luminance until refresh. Quantizing here would destroy the
    // coverage produced by anti-aliased fonts and would make a later ordered or
    // error-diffusion dither impossible to implement correctly.
    const size_t luma_idx = static_cast<size_t>(y) * epd.getDisplayWidth() + x;
    // Native display drawing uses COLOR_ON (255) as ink and COLOR_OFF (0) as
    // paper, the inverse of literal RGB luminance. Preserve intermediate native
    // values as coverage so multi-bpp ESPHome fonts remain anti-aliased too.
    const uint8_t output_luma =
        this->rgb_input_ ? static_cast<uint8_t>(luma) : static_cast<uint8_t>(255U - luma);
    this->buffer_dirty_ |= this->gray_luma_[luma_idx] != output_luma;
    this->gray_luma_[luma_idx] = output_luma;
  }
}

void XteinkDisplay::render_grayscale_planes_() {
  auto &epd = this->parent_->display();
  memset(this->gray_lsb_, 0, this->gray_buffer_size_);
  memset(this->gray_msb_, 0, this->gray_buffer_size_);
  for (int y = 0; y < epd.getDisplayHeight(); y++) {
    for (int x = 0; x < epd.getDisplayWidth(); x++) {
      const size_t luma_idx = static_cast<size_t>(y) * epd.getDisplayWidth() + x;
      const size_t plane_idx = static_cast<size_t>(y) * epd.getDisplayWidthBytes() + (x >> 3);
      const uint8_t bit = 0x80 >> (x & 7);
      // Current output is the panel's native four levels. Keeping quantization
      // in this one full-frame stage lets a future dither select among those
      // levels using the original 8-bit luminance and neighboring pixels.
      const uint8_t level = static_cast<uint8_t>((this->gray_luma_[luma_idx] * 3U + 127U) / 255U);
      if (level & 1)
        this->gray_lsb_[plane_idx] |= bit;
      if (level & 2)
        this->gray_msb_[plane_idx] |= bit;
    }
  }
}

void XteinkDisplay::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                                   display::ColorOrder order, display::ColorBitness bitness, bool big_endian,
                                   int x_offset, int y_offset, int x_pad) {
  this->rgb_input_ = true;
  display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset, x_pad);
  this->rgb_input_ = false;
}

void XteinkDisplay::update() {
  auto &epd = this->parent_->display();
  if (this->writer_local_.has_value()) {
    // Lambda/pages rendering is a complete-frame API by default. Clear before
    // drawing, never after displaying: LVGL has no writer and patches only its
    // invalidated regions into the persistent framebuffer.
    if (this->auto_clear_enabled_) {
      epd.clearScreen(0xFF);
      if (this->grayscale_available_) {
        memset(this->gray_luma_, 0xFF, this->gray_luma_size_);
      }
      this->buffer_dirty_ = true;
    }
    (*this->writer_local_)(*this);
  }
  if (!this->buffer_dirty_) {
    ESP_LOGV(TAG, "Skipping panel refresh: framebuffer is unchanged");
    return;
  }

  EInkDisplay::RefreshMode mode = EInkDisplay::FULL_REFRESH;
  if (this->refresh_mode_ == HALF_REFRESH || (this->refresh_mode_ == FAST_REFRESH && !this->has_drawn_)) {
    mode = EInkDisplay::HALF_REFRESH;  // the very first frame cannot be a fast (diff) refresh
  } else if (this->refresh_mode_ == FAST_REFRESH) {
    mode = EInkDisplay::FAST_REFRESH;
  }

  const bool periodic_full = this->full_refresh_every_ != 0 &&
                             (this->update_count + 1) % this->full_refresh_every_ == 0;
  if (periodic_full)
    mode = EInkDisplay::FULL_REFRESH;

  bool grayscale_rendered = false;
  if (this->grayscale_available_) {
    this->render_grayscale_planes_();
    if (epd.displayGrayscaleBase(freeink::GrayscaleMode::Absolute, mode)) {
      epd.copyGrayscaleBuffers(this->gray_lsb_, this->gray_msb_);
      epd.displayGrayBuffer();
      epd.cleanupGrayscaleBuffers(epd.getFrameBuffer());
      grayscale_rendered = true;
    } else {
      if (!this->grayscale_warning_logged_) {
        ESP_LOGW(TAG, "Grayscale became unavailable; using monochrome");
        this->grayscale_warning_logged_ = true;
      }
      epd.displayBuffer(mode);
    }
  } else {
    epd.displayBuffer(mode);
  }
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Grayscale does not pass through displayBuffer(), so it does not swap the
  // two FreeInk buffers. Make the rendered frame active explicitly; the copy
  // below then gives LVGL an identical persistent write surface.
  if (grayscale_rendered)
    epd.swapBuffers();
#else
  (void) grayscale_rendered;
#endif
  // In dual-buffer mode displayBuffer() swaps current and previous frames.
  // LVGL only patches dirty regions, so seed its new write surface from the
  // just-displayed frame. This is a no-op on the C3 single-buffer builds.
  epd.syncWriteBufferFromActive();
  this->has_drawn_ = true;
  this->update_count++;
  this->buffer_dirty_ = false;
}

void XteinkDisplay::dump_config() {
  LOG_DISPLAY("", "Xteink e-paper", this);
  ESP_LOGCONFIG(TAG, "  Refresh mode: %s", this->refresh_mode_ == FULL_REFRESH ? "full" :
                                               this->refresh_mode_ == FAST_REFRESH ? "fast" : "half");
  ESP_LOGCONFIG(TAG, "  Full refresh every: %u", static_cast<unsigned>(this->full_refresh_every_));
  ESP_LOGCONFIG(TAG, "  Grayscale requested: %s", YESNO(this->grayscale_enabled_));
  ESP_LOGCONFIG(TAG, "  Grayscale available: %s", YESNO(this->grayscale_available_));
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace xteink
}  // namespace esphome
