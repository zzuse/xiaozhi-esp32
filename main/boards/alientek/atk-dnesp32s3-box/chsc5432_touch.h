#ifndef CHSC5432_TOUCH_H
#define CHSC5432_TOUCH_H

#include <driver/i2c_master.h>
#include <lvgl.h>

#include <functional>

// CHSC5432 capacitive touch controller (32-bit register addresses).
// Coordinates are reported for the landscape orientation used by this board.
class Chsc5432Touch {
public:
    Chsc5432Touch(i2c_master_bus_handle_t i2c_bus, uint8_t addr, int width, int height);
    ~Chsc5432Touch();

    // Reset must be driven by the caller (the reset line sits on the IO expander).
    bool Probe();
    bool ReadPoint(int& x, int& y);
    lv_indev_t* AttachToLvgl(lv_display_t* display);

private:
    i2c_master_dev_handle_t dev_ = nullptr;
    int width_;
    int height_;
    int last_x_ = 0;
    int last_y_ = 0;

    esp_err_t ReadReg(uint32_t reg, uint8_t* buf, size_t len);
    static void LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
};

#endif  // CHSC5432_TOUCH_H
