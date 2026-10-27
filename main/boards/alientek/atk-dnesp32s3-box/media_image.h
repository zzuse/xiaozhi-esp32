#ifndef MEDIA_IMAGE_H
#define MEDIA_IMAGE_H

#include <esp_jpeg_dec.h>
#include <lvgl.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// RGB565 (LVGL native byte order) pixel buffer in PSRAM, usable as an LVGL image source.
class Rgb565Image {
public:
    Rgb565Image() = default;
    ~Rgb565Image();
    Rgb565Image(const Rgb565Image&) = delete;
    Rgb565Image& operator=(const Rgb565Image&) = delete;

    // Reuses the current buffer when it is large enough.
    bool Reserve(int width, int height);
    void SetSize(int width, int height);
    uint8_t* data() { return data_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const lv_image_dsc_t* dsc() const { return &dsc_; }

private:
    uint8_t* data_ = nullptr;
    size_t capacity_ = 0;
    int width_ = 0;
    int height_ = 0;
    lv_image_dsc_t dsc_ = {};
};

// Reusable JPEG decoder for still images and MJPEG frames.
// The decoder scales down by up to 1/8 to fit max_width x max_height.
class JpegDecoder {
public:
    ~JpegDecoder();
    bool Decode(const uint8_t* data, size_t len, int max_width, int max_height, Rgb565Image& out);

private:
    jpeg_dec_handle_t handle_ = nullptr;
    int scale_width_ = 0;
    int scale_height_ = 0;

    bool Open(int scale_width, int scale_height);
    void CloseHandle();
};

// Decodes a JPG/PNG/BMP file and fits it into max_width x max_height.
// PNG decoding uses LVGL's allocator, so call it with the LVGL lock held.
std::unique_ptr<Rgb565Image> DecodeImageFile(const std::string& path, int max_width, int max_height,
                                             std::string& error);
bool IsPngFile(const std::string& path);

#endif  // MEDIA_IMAGE_H
