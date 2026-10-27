#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "chsc5432_touch.h"
#include "desktop_display.h"
#include "esp_video.h"

#include <atomic>
#include <driver/sdspi_host.h>
#include <driver/spi_master.h>
#include <esp_lvgl_port.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#endif

#define TAG "atk_dnesp32s3_box"

class ATK_NoAudioCodecDuplex : public NoAudioCodec {
public:
    ATK_NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
        duplex_ = true;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
    
        i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
        };
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));
    
        i2s_std_config_t std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
#endif
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                .ws_pol = false,
                .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
#endif
            },
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = bclk,
                .ws = ws,
                .dout = dout,
                .din = din,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false
                }
            }
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
        ESP_LOGI(TAG, "Duplex channels created");
    }
};

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
// Lets desktop media playback keep the speaker powered: AudioService turns the output
// off after it has been idle for a while, which would cut off music and video sound.
template <typename Base>
class MediaHoldCodec : public Base {
public:
    using Base::Base;

    void SetMediaOutputHold(bool hold) { hold_ = hold; }

    void EnableOutput(bool enable) override {
        if (!enable && hold_) {
            return;
        }
        Base::EnableOutput(enable);
    }

private:
    std::atomic<bool> hold_{false};
};

using BoxEs8311AudioCodec = MediaHoldCodec<Es8311AudioCodec>;
using BoxNs4168AudioCodec = MediaHoldCodec<ATK_NoAudioCodecDuplex>;
#else
using BoxEs8311AudioCodec = Es8311AudioCodec;
using BoxNs4168AudioCodec = ATK_NoAudioCodecDuplex;
#endif

class XL9555_IN : public I2cDevice {
public:
    XL9555_IN(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x06, 0x3B);
        WriteReg(0x07, 0xFE);
    }

    void xl9555_cfg(void) {
        WriteReg(0x06, 0x1B);
        WriteReg(0x07, 0xFE);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint16_t data;
        int index = bit;

        if (bit < 8) {
            data = ReadReg(0x02);
        } else {
            data = ReadReg(0x03);
            index -= 8;
        }

        data = (data & ~(1 << index)) | (level << index);

        if (bit < 8) {
            WriteReg(0x02, data);
        } else {
            WriteReg(0x03, data);
        }
    }

    int GetPingState(uint16_t pin) {
        uint8_t data;
        if (pin <= 0x0080) {
            data = ReadReg(0x00);
            return (data & (uint8_t)(pin & 0xFF)) ? 1 : 0;
        } else {
            data = ReadReg(0x01);
            return (data & (uint8_t)((pin >> 8) & 0xFF )) ? 1 : 0;
        }

        return 0;
    }
};

class atk_dnesp32s3_box : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t xl9555_handle_;
    Button boot_button_;
    LcdDisplay* display_;
    XL9555_IN* xl9555_in_;
    bool es8311_detected_ = false;
#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
    Chsc5432Touch* touch_ = nullptr;
    Camera* camera_ = nullptr;
    sdmmc_card_t* tf_card_ = nullptr;
    std::atomic<bool> tf_mounted_{false};
    bool tf_spi_ready_ = false;
    std::function<void(bool)> media_output_hold_;

    DesktopServices CreateDesktopServices() {
        DesktopServices services;
        services.audio_codec = [this]() { return GetAudioCodec(); };
        services.set_media_output_hold = [this](bool hold) {
            GetAudioCodec();
            media_output_hold_(hold);
        };
        services.is_tf_mounted = [this]() { return tf_mounted_.load(); };
        services.ensure_tf_mounted = [this]() { return MountTfCard(); };
        return services;
    }

    bool MountTfCard() {
        // Once mounted, removal is left to the file APIs: probing the card here could
        // interleave with reads from an app worker on the same SPI device.
        if (tf_mounted_) {
            return true;
        }
        if (!tf_spi_ready_) {
            spi_bus_config_t bus_cfg = {};
            bus_cfg.mosi_io_num = TF_SPI_MOSI;
            bus_cfg.miso_io_num = TF_SPI_MISO;
            bus_cfg.sclk_io_num = TF_SPI_SCLK;
            bus_cfg.quadwp_io_num = GPIO_NUM_NC;
            bus_cfg.quadhd_io_num = GPIO_NUM_NC;
            bus_cfg.max_transfer_sz = 4096;
            if (spi_bus_initialize(TF_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
                ESP_LOGE(TAG, "TF card SPI bus init failed");
                return false;
            }
            tf_spi_ready_ = true;
        }

        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = TF_SPI_HOST;
        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_cfg.gpio_cs = TF_SPI_CS;
        slot_cfg.host_id = TF_SPI_HOST;
        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
        mount_cfg.format_if_mount_failed = false;
        mount_cfg.max_files = 5;
        mount_cfg.allocation_unit_size = 16 * 1024;
        esp_err_t err = esp_vfs_fat_sdspi_mount(TF_MOUNT_POINT, &host, &slot_cfg, &mount_cfg,
                                                &tf_card_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TF card not mounted: %s", esp_err_to_name(err));
            tf_card_ = nullptr;
            return false;
        }
        ESP_LOGI(TAG, "TF card mounted at %s", TF_MOUNT_POINT);
        tf_mounted_ = true;
        return true;
    }

    void InitializeTouch() {
        xl9555_in_->SetOutputState(XL9555_BIT_CTP_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        xl9555_in_->SetOutputState(XL9555_BIT_CTP_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        if (i2c_master_probe(i2c_bus_, TOUCH_CHSC5432_ADDR, 50) != ESP_OK) {
            ESP_LOGW(TAG, "No CHSC5432 touch panel found");
            return;
        }
        touch_ = new Chsc5432Touch(i2c_bus_, TOUCH_CHSC5432_ADDR, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (!touch_->Probe()) {
            return;
        }
        lvgl_port_lock(0);
        touch_->AttachToLvgl(lv_display_get_default());
        lvgl_port_unlock();
    }

    void InitializeCamera() {
        // Waits up to CONFIG_USB_UVC_INIT_TIMEOUT_MS for a UVC camera on the USB port
        esp_video_init_usb_uvc_config_t usb_uvc_config = {
            .uvc = {
                .uvc_dev_num = 1,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
            .usb = {
                .init_usb_host_lib = true,
                .task_stack = 4096,
                .task_priority = 5,
                .task_affinity = -1,
            },
        };
        esp_video_init_config_t video_config = {
            .usb_uvc = &usb_uvc_config,
        };
        camera_ = new EspVideo(video_config);
    }
#endif

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = GPIO_NUM_48,
            .scl_io_num = GPIO_NUM_45,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize XL9555
        xl9555_in_ = new XL9555_IN(i2c_bus_, 0x20);

        if (xl9555_in_->GetPingState(0x0020) == 1) {
            es8311_detected_ = true;    /* 音频设备标志位，SPK_CTRL_IO为高电平时，该标志位置1，且判定为ES8311 */
        } else {
            es8311_detected_ = false;    /* 音频设备标志位，SPK_CTRL_IO为低电平时，该标志位置0，且判定为NS4168 */
        }

        xl9555_in_->xl9555_cfg();
    }

    void InitializeATK_ST7789_80_Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        /* 配置RD引脚 */
        gpio_config_t gpio_init_struct;
        gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
        gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
        gpio_init_struct.pin_bit_mask = 1ull << LCD_NUM_RD;
        gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&gpio_init_struct);
        gpio_set_level(LCD_NUM_RD, 1);

        esp_lcd_i80_bus_handle_t i80_bus = NULL;
        esp_lcd_i80_bus_config_t bus_config = {
            .dc_gpio_num = LCD_NUM_DC,
            .wr_gpio_num = LCD_NUM_WR,
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .data_gpio_nums = {
                GPIO_LCD_D0,
                GPIO_LCD_D1,
                GPIO_LCD_D2,
                GPIO_LCD_D3,
                GPIO_LCD_D4,
                GPIO_LCD_D5,
                GPIO_LCD_D6,
                GPIO_LCD_D7,
            },
            .bus_width = 8,
            .max_transfer_bytes = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .dma_burst_size = 64,
        };
        ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

        esp_lcd_panel_io_i80_config_t io_config = {
            .cs_gpio_num = LCD_NUM_CS,
            .pclk_hz = (10 * 1000 * 1000),
            .trans_queue_depth = 10,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .dc_levels = {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
            .flags = {
                .swap_color_bytes = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.reset_gpio_num = LCD_NUM_RST;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        esp_lcd_panel_set_gap(panel, 0, 0);
        uint8_t data0[] = {0x00};
        uint8_t data1[] = {0x65};
        esp_lcd_panel_io_tx_param(panel_io, 0x36, data0, 1);
        esp_lcd_panel_io_tx_param(panel_io, 0x3A, data1, 1);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
        display_ = new DesktopDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    CreateDesktopServices());
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    atk_dnesp32s3_box() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeATK_ST7789_80_Display();
        xl9555_in_->SetOutputState(5, 1);
        xl9555_in_->SetOutputState(7, 1);
        InitializeButtons();
#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
        InitializeTouch();
        MountTfCard();
        InitializeCamera();
#endif
    }

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
    virtual Camera* GetCamera() override {
        return camera_;
    }
#endif

    virtual AudioCodec* GetAudioCodec() override {
        /* 根据探测结果初始化编解码器 */
        if (es8311_detected_) {
            /* 使用ES8311 驱动 */
            static BoxEs8311AudioCodec audio_codec(
                i2c_bus_, 
                I2C_NUM_0, 
                AUDIO_INPUT_SAMPLE_RATE,
                AUDIO_OUTPUT_SAMPLE_RATE,
                GPIO_NUM_NC, 
                AUDIO_I2S_GPIO_BCLK, 
                AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT,
                AUDIO_I2S_GPIO_DIN,
                GPIO_NUM_NC,
                AUDIO_CODEC_ES8311_ADDR,
                false);
#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
                if (!media_output_hold_) {
                    media_output_hold_ = [](bool hold) { audio_codec.SetMediaOutputHold(hold); };
                }
#endif
                return &audio_codec;
        } else {
            static BoxNs4168AudioCodec audio_codec(
                AUDIO_INPUT_SAMPLE_RATE,
                AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_BCLK,
                AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT,
                AUDIO_I2S_GPIO_DIN);
#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
                if (!media_output_hold_) {
                    media_output_hold_ = [](bool hold) { audio_codec.SetMediaOutputHold(hold); };
                }
#endif
                return &audio_codec;
        }
        return NULL;
    }
    
    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(atk_dnesp32s3_box);
