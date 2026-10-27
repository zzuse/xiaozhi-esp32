#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

#include "application.h"
#include "board.h"
#include "camera.h"
#include "desktop_app.h"
#include "device_state.h"
#include "jpg/image_to_jpeg.h"
#include "lvgl_image.h"

#define TAG "CameraApp"

static constexpr const char* kPhotoDir = "/sdcard/PICTURE";
static constexpr int kRetryMs = 1000;

namespace {

struct CameraState {
    UiSession session;
    std::atomic<bool> shutter{false};
    lv_obj_t* image = nullptr;
    lv_obj_t* status = nullptr;
    int max_width = 0;
    int max_height = 0;
    // Written by the worker inside Capture() with the LVGL lock held
    std::unique_ptr<LvglImage> frame;
};

class CameraApp : public DesktopApp {
public:
    explicit CameraApp(const DesktopServices& services) : services_(services) {}

    void Open(lv_obj_t* screen) override {
        state_ = std::make_shared<CameraState>();
        auto camera = Board::GetInstance().GetCamera();
        if (camera == nullptr) {
            CreateNotice(screen, services_.text_font, "Camera is not available");
            return;
        }
        state_->max_width = lv_obj_get_width(screen);
        state_->max_height = lv_obj_get_height(screen);

        state_->image = lv_image_create(screen);
        lv_obj_center(state_->image);

        state_->status = CreateNotice(screen, services_.text_font, "Starting USB camera...");

        auto shutter = lv_button_create(screen);
        lv_obj_set_size(shutter, 52, 52);
        lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(shutter, lv_color_white(), 0);
        lv_obj_set_style_border_color(shutter, lv_color_hex(0x22B14C), 0);
        lv_obj_set_style_border_width(shutter, 4, 0);
        lv_obj_set_style_bg_opa(shutter, LV_OPA_70, LV_STATE_PRESSED);
        lv_obj_align(shutter, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(shutter, OnShutter, LV_EVENT_CLICKED, this);

        auto state = state_;
        auto services = &services_;
        StartWorkerTask("camera_app", 8192, 3,
                        [state, camera, services]() { Run(state, camera, *services); });
    }

    void Close() override {
        if (state_) {
            state_->session.Cancel();
        }
    }

    // Called by the display with the LVGL lock held, from the worker inside Capture()
    bool ConsumePreviewImage(std::unique_ptr<LvglImage>& image) override {
        if (!state_ || state_->session.cancelled() || image == nullptr) {
            return false;
        }
        auto dsc = image->image_dsc();
        lv_image_cache_drop(dsc);
        lv_image_set_src(state_->image, dsc);
        if (dsc->header.w > 0 && dsc->header.h > 0) {
            float scale = std::min(static_cast<float>(state_->max_width) / dsc->header.w,
                                   static_cast<float>(state_->max_height) / dsc->header.h);
            lv_image_set_scale(state_->image, static_cast<uint32_t>(LV_SCALE_NONE * scale));
        }
        if (state_->frame) {
            lv_image_cache_drop(state_->frame->image_dsc());
        }
        state_->frame = std::move(image);
        lv_obj_add_flag(state_->status, LV_OBJ_FLAG_HIDDEN);
        return true;
    }

private:
    const DesktopServices& services_;
    std::shared_ptr<CameraState> state_;

    static void OnShutter(lv_event_t* e) {
        auto self = static_cast<CameraApp*>(lv_event_get_user_data(e));
        self->state_->shutter = true;
    }

    static void SetStatus(std::shared_ptr<CameraState>& state, const char* text) {
        state->session.RunLocked([&]() {
            lv_label_set_text(state->status, text);
            lv_obj_remove_flag(state->status, LV_OBJ_FLAG_HIDDEN);
        });
    }

    static void Run(std::shared_ptr<CameraState> state, Camera* camera,
                    const DesktopServices& services) {
        auto& app = Application::GetInstance();
        while (!state->session.cancelled()) {
            // The AI may use the camera during a conversation; the desktop closes this
            // app as soon as one starts, so only capture while idle.
            if (app.GetDeviceState() != kDeviceStateIdle) {
                state->session.Sleep(100);
                continue;
            }
            if (!camera->Capture()) {
                SetStatus(state, "No USB camera detected.\nConnect a UVC camera and restart.");
                state->session.Sleep(kRetryMs);
                continue;
            }
            if (state->shutter.exchange(false)) {
                SaveSnapshot(state, services);
            }
        }
    }

    static void SaveSnapshot(std::shared_ptr<CameraState>& state, const DesktopServices& services) {
        // state->frame is only replaced by this task (inside Capture), so it is stable here
        auto dsc = state->frame ? state->frame->image_dsc() : nullptr;
        if (dsc == nullptr || dsc->header.cf != LV_COLOR_FORMAT_RGB565) {
            return;
        }
        if (!services.is_tf_mounted()) {
            SetStatus(state, "Insert a TF card to save photos");
            return;
        }
        uint8_t* jpeg = nullptr;
        size_t jpeg_len = 0;
        if (!image_to_jpeg(const_cast<uint8_t*>(dsc->data), dsc->data_size, dsc->header.w,
                           dsc->header.h, V4L2_PIX_FMT_RGB565, 85, &jpeg, &jpeg_len)) {
            ESP_LOGE(TAG, "JPEG encoding failed");
            return;
        }
        mkdir(kPhotoDir, 0775);
        char path[64];
        for (int i = 1; i < 10000; i++) {
            snprintf(path, sizeof(path), "%s/IMG_%04d.jpg", kPhotoDir, i);
            struct stat st;
            if (stat(path, &st) != 0) {
                break;
            }
        }
        FILE* f = fopen(path, "wb");
        if (f != nullptr) {
            fwrite(jpeg, 1, jpeg_len, f);
            fclose(f);
            ESP_LOGI(TAG, "Saved %s (%u bytes)", path, static_cast<unsigned>(jpeg_len));
        }
        heap_caps_free(jpeg);
    }
};

}  // namespace

std::unique_ptr<DesktopApp> CreateCameraApp(const DesktopServices& services) {
    return std::make_unique<CameraApp>(services);
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
