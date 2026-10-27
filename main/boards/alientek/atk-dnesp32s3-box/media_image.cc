#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "media_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "desktop_app.h"

#define TAG "MediaImage"

// From LVGL's bundled lodepng (CONFIG_LV_USE_LODEPNG). Its header cannot be included from
// C++ because it pulls in <vector> inside an extern "C" block.
extern "C" unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                                     const unsigned char* in, size_t insize);

static constexpr size_t kMaxImageFileSize = 8 * 1024 * 1024;
// lodepng allocates the RGBA output from the LVGL heap
static constexpr uint32_t kMaxPngPixels = 800 * 600;
static constexpr uint32_t kMaxBmpWidth = 4096;

static inline uint16_t ToRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

Rgb565Image::~Rgb565Image() {
    if (data_ != nullptr) {
        heap_caps_free(data_);
    }
}

bool Rgb565Image::Reserve(int width, int height) {
    size_t size = static_cast<size_t>(width) * height * 2;
    if (size <= capacity_) {
        return true;
    }
    if (data_ != nullptr) {
        heap_caps_free(data_);
    }
    // esp_new_jpeg requires a 16-byte aligned output buffer
    data_ = static_cast<uint8_t*>(
        heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    capacity_ = data_ != nullptr ? size : 0;
    if (data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %ux%u image", width, height);
        return false;
    }
    return true;
}

void Rgb565Image::SetSize(int width, int height) {
    width_ = width;
    height_ = height;
    dsc_ = {};
    dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc_.header.w = width;
    dsc_.header.h = height;
    dsc_.header.stride = width * 2;
    dsc_.data_size = static_cast<uint32_t>(width) * height * 2;
    dsc_.data = data_;
}

static void FitSize(int width, int height, int max_width, int max_height, int& out_width,
                    int& out_height) {
    float scale = std::min(
        {1.0f, static_cast<float>(max_width) / width, static_cast<float>(max_height) / height});
    out_width = std::max(1, static_cast<int>(width * scale));
    out_height = std::max(1, static_cast<int>(height * scale));
}

// Nearest-neighbour resample of src into a new image that fits max_width x max_height
template <typename PixelFn>
static std::unique_ptr<Rgb565Image> Resample(int width, int height, int max_width, int max_height,
                                             PixelFn pixel) {
    int out_width, out_height;
    FitSize(width, height, max_width, max_height, out_width, out_height);
    auto out = std::make_unique<Rgb565Image>();
    if (!out->Reserve(out_width, out_height)) {
        return nullptr;
    }
    auto dst = reinterpret_cast<uint16_t*>(out->data());
    for (int y = 0; y < out_height; y++) {
        int sy = y * height / out_height;
        for (int x = 0; x < out_width; x++) {
            dst[y * out_width + x] = pixel(x * width / out_width, sy);
        }
    }
    out->SetSize(out_width, out_height);
    return out;
}

JpegDecoder::~JpegDecoder() { CloseHandle(); }

void JpegDecoder::CloseHandle() {
    if (handle_ != nullptr) {
        jpeg_dec_close(handle_);
        handle_ = nullptr;
    }
}

bool JpegDecoder::Open(int scale_width, int scale_height) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.scale.width = scale_width;
    config.scale.height = scale_height;
    if (jpeg_dec_open(&config, &handle_) != JPEG_ERR_OK) {
        handle_ = nullptr;
        return false;
    }
    scale_width_ = scale_width;
    scale_height_ = scale_height;
    return true;
}

// The decoder scales by up to 1/8, in steps that keep both sides multiples of 8
static void ComputeScale(int width, int height, int max_width, int max_height, int& scale_width,
                         int& scale_height) {
    scale_width = scale_height = 0;
    if (width <= max_width && height <= max_height) {
        return;
    }
    float factor = std::max(
        std::min(static_cast<float>(max_width) / width, static_cast<float>(max_height) / height),
        0.125f);
    int w = static_cast<int>(width * factor) & ~7;
    int h = static_cast<int>(height * factor) & ~7;
    while (w * 8 < width) {
        w += 8;
    }
    while (h * 8 < height) {
        h += 8;
    }
    if (w > 0 && h > 0 && w < width && h < height) {
        scale_width = w;
        scale_height = h;
    }
}

bool JpegDecoder::Decode(const uint8_t* data, size_t len, int max_width, int max_height,
                         Rgb565Image& out) {
    if (handle_ == nullptr && !Open(scale_width_, scale_height_)) {
        return false;
    }
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = const_cast<uint8_t*>(data);
    io.inbuf_len = static_cast<int>(len);
    if (jpeg_dec_parse_header(handle_, &io, &info) != JPEG_ERR_OK) {
        CloseHandle();
        return false;
    }

    int scale_width, scale_height;
    ComputeScale(info.width, info.height, max_width, max_height, scale_width, scale_height);
    if (scale_width != scale_width_ || scale_height != scale_height_) {
        CloseHandle();
        if (!Open(scale_width, scale_height)) {
            return false;
        }
        io = {};
        io.inbuf = const_cast<uint8_t*>(data);
        io.inbuf_len = static_cast<int>(len);
        if (jpeg_dec_parse_header(handle_, &io, &info) != JPEG_ERR_OK) {
            CloseHandle();
            return false;
        }
    }

    int out_width = scale_width ? scale_width : info.width;
    int out_height = scale_height ? scale_height : info.height;
    int outbuf_len = 0;
    jpeg_dec_get_outbuf_len(handle_, &outbuf_len);
    if (outbuf_len > out_width * out_height * 2) {
        ESP_LOGE(TAG, "Unexpected JPEG output size %d", outbuf_len);
        CloseHandle();
        return false;
    }
    if (!out.Reserve(out_width, out_height)) {
        return false;
    }

    io.outbuf = out.data();
    int consumed = io.inbuf_len - io.inbuf_remain;
    io.inbuf += consumed;
    io.inbuf_len = io.inbuf_remain;
    if (jpeg_dec_process(handle_, &io) != JPEG_ERR_OK) {
        CloseHandle();
        return false;
    }
    out.SetSize(out_width, out_height);
    return true;
}

static uint8_t* ReadWholeFile(const std::string& path, size_t& size, std::string& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "Cannot open file";
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size <= 0 || static_cast<size_t>(file_size) > kMaxImageFileSize) {
        fclose(f);
        error = "File too large";
        return nullptr;
    }
    auto buf = static_cast<uint8_t*>(heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        fclose(f);
        error = "Out of memory";
        return nullptr;
    }
    size = fread(buf, 1, file_size, f);
    fclose(f);
    return buf;
}

static std::unique_ptr<Rgb565Image> DecodeJpegFile(const std::string& path, int max_width,
                                                   int max_height, std::string& error) {
    size_t size = 0;
    uint8_t* buf = ReadWholeFile(path, size, error);
    if (buf == nullptr) {
        return nullptr;
    }
    JpegDecoder decoder;
    auto image = std::make_unique<Rgb565Image>();
    bool ok = decoder.Decode(buf, size, max_width, max_height, *image);
    heap_caps_free(buf);
    if (!ok) {
        error = "Unsupported JPEG";
        return nullptr;
    }
    if (image->width() <= max_width && image->height() <= max_height) {
        return image;
    }
    // Larger than 8x the screen: finish the reduction in software
    auto src = reinterpret_cast<const uint16_t*>(image->data());
    int src_width = image->width();
    return Resample(image->width(), image->height(), max_width, max_height,
                    [&](int x, int y) { return src[y * src_width + x]; });
}

static std::unique_ptr<Rgb565Image> DecodePngFile(const std::string& path, int max_width,
                                                  int max_height, std::string& error) {
    size_t size = 0;
    uint8_t* buf = ReadWholeFile(path, size, error);
    if (buf == nullptr) {
        return nullptr;
    }
    // IHDR width/height are big-endian at offsets 16 and 20
    if (size < 24) {
        heap_caps_free(buf);
        error = "Invalid PNG";
        return nullptr;
    }
    uint32_t width = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
    uint32_t height = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
    if (width == 0 || height == 0 || width * height > kMaxPngPixels) {
        heap_caps_free(buf);
        error = "PNG too large (max 800x600)";
        return nullptr;
    }

    unsigned char* rgba = nullptr;
    unsigned w = 0, h = 0;
    unsigned ret = lodepng_decode32(&rgba, &w, &h, buf, size);
    heap_caps_free(buf);
    if (ret != 0 || rgba == nullptr) {
        error = "Unsupported PNG";
        return nullptr;
    }
    // Blend transparent pixels over black
    auto image = Resample(w, h, max_width, max_height, [&](int x, int y) {
        const uint8_t* p = rgba + (y * w + x) * 4;
        return ToRgb565(p[0] * p[3] / 255, p[1] * p[3] / 255, p[2] * p[3] / 255);
    });
    lv_free(rgba);
    if (image == nullptr) {
        error = "Out of memory";
    }
    return image;
}

static std::unique_ptr<Rgb565Image> DecodeBmpFile(const std::string& path, int max_width,
                                                  int max_height, std::string& error) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "Cannot open file";
        return nullptr;
    }
    uint8_t header[54];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || header[0] != 'B' ||
        header[1] != 'M') {
        fclose(f);
        error = "Invalid BMP";
        return nullptr;
    }
    auto le32 = [&](int off) {
        return static_cast<uint32_t>(header[off] | (header[off + 1] << 8) |
                                     (header[off + 2] << 16) | (header[off + 3] << 24));
    };
    uint32_t data_offset = le32(10);
    int32_t width = static_cast<int32_t>(le32(18));
    int32_t height = static_cast<int32_t>(le32(22));
    uint16_t bpp = header[28] | (header[29] << 8);
    uint32_t compression = le32(30);
    bool top_down = height < 0;
    height = std::abs(height);
    bool rgb565 = bpp == 16 && compression == 3;  // BI_BITFIELDS, assume 565 masks
    if (width <= 0 || height == 0 || static_cast<uint32_t>(width) > kMaxBmpWidth ||
        !(bpp == 16 || bpp == 24 || bpp == 32) || !(compression == 0 || compression == 3)) {
        fclose(f);
        error = "Unsupported BMP format";
        return nullptr;
    }

    uint32_t row_size = ((width * bpp + 31) / 32) * 4;
    auto row = static_cast<uint8_t*>(heap_caps_malloc(row_size, MALLOC_CAP_SPIRAM));
    if (row == nullptr) {
        fclose(f);
        error = "Out of memory";
        return nullptr;
    }
    int cached_row = -1;
    auto image = Resample(width, height, max_width, max_height, [&](int x, int y) -> uint16_t {
        if (y != cached_row) {
            int file_row = top_down ? y : height - 1 - y;
            fseek(f, data_offset + static_cast<long>(file_row) * row_size, SEEK_SET);
            if (fread(row, 1, row_size, f) != row_size) {
                memset(row, 0, row_size);
            }
            cached_row = y;
        }
        if (bpp == 16) {
            uint16_t v = row[x * 2] | (row[x * 2 + 1] << 8);
            if (rgb565) {
                return v;
            }
            // X1R5G5B5
            return ((v & 0x7FE0) << 1) | (v & 0x1F);
        }
        const uint8_t* p = row + x * (bpp / 8);
        return ToRgb565(p[2], p[1], p[0]);
    });
    heap_caps_free(row);
    fclose(f);
    if (image == nullptr) {
        error = "Out of memory";
    }
    return image;
}

bool IsPngFile(const std::string& path) { return HasExtension(path, "png"); }

std::unique_ptr<Rgb565Image> DecodeImageFile(const std::string& path, int max_width, int max_height,
                                             std::string& error) {
    if (HasExtension(path, "jpg") || HasExtension(path, "jpeg")) {
        return DecodeJpegFile(path, max_width, max_height, error);
    }
    if (IsPngFile(path)) {
        return DecodePngFile(path, max_width, max_height, error);
    }
    if (HasExtension(path, "bmp")) {
        return DecodeBmpFile(path, max_width, max_height, error);
    }
    error = "Unsupported file type";
    return nullptr;
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
