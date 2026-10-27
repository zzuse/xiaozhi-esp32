#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "chsc5432_touch.h"

#include <esp_log.h>

#define TAG "Chsc5432"

static constexpr uint32_t kRegTouchEvent = 0x2000002C;
static constexpr uint32_t kRegProductId = 0x20000080;
static constexpr int kI2cTimeoutMs = 50;

Chsc5432Touch::Chsc5432Touch(i2c_master_bus_handle_t i2c_bus, uint8_t addr, int width, int height)
    : width_(width), height_(height) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100 * 1000,  // speed proven by the vendor demo on this shared bus
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_));
}

Chsc5432Touch::~Chsc5432Touch() {
    if (dev_ != nullptr) {
        i2c_master_bus_rm_device(dev_);
    }
}

esp_err_t Chsc5432Touch::ReadReg(uint32_t reg, uint8_t* buf, size_t len) {
    uint8_t addr[4] = {
        static_cast<uint8_t>(reg >> 24),
        static_cast<uint8_t>(reg >> 16),
        static_cast<uint8_t>(reg >> 8),
        static_cast<uint8_t>(reg),
    };
    return i2c_master_transmit_receive(dev_, addr, sizeof(addr), buf, len, kI2cTimeoutMs);
}

bool Chsc5432Touch::Probe() {
    uint8_t pid[4] = {};
    if (ReadReg(kRegProductId, pid, sizeof(pid)) != ESP_OK) {
        ESP_LOGW(TAG, "Touch controller not responding");
        return false;
    }
    ESP_LOGI(TAG, "Touch product id: 0x%02x", pid[0]);
    return true;
}

bool Chsc5432Touch::ReadPoint(int& x, int& y) {
    // The vendor recommends reading the whole 28-byte event block at once.
    uint8_t buf[28];
    if (ReadReg(kRegTouchEvent, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    int points = buf[1] & 0x0F;
    if (points == 0 || points > 5) {
        return false;
    }
    int raw_x = ((buf[5] >> 4) << 8) | buf[3];
    int raw_y = ((buf[5] & 0x0F) << 8) | buf[2];
    raw_y = height_ - raw_y;
    if (raw_x < 0 || raw_x >= width_ || raw_y < 0 || raw_y >= height_) {
        return false;
    }
    x = raw_x;
    y = raw_y;
    return true;
}

void Chsc5432Touch::LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data) {
    auto self = static_cast<Chsc5432Touch*>(lv_indev_get_user_data(indev));
    int x, y;
    if (self->ReadPoint(x, y)) {
        self->last_x_ = x;
        self->last_y_ = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = self->last_x_;
    data->point.y = self->last_y_;
}

lv_indev_t* Chsc5432Touch::AttachToLvgl(lv_display_t* display) {
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, display);
    lv_indev_set_user_data(indev, this);
    lv_indev_set_read_cb(indev, LvglReadCallback);
    return indev;
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
