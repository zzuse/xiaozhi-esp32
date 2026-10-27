#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include <esp_log.h>

#include "desktop_app.h"
#include "media_image.h"

#define TAG "PhotoApp"

LV_IMAGE_DECLARE(desktop_img_prev);
LV_IMAGE_DECLARE(desktop_img_next);

static constexpr const char* kPictureDir = "/sdcard/PICTURE";

namespace {

struct PhotoState {
    UiSession session;
    std::vector<std::string> files;
    std::atomic<int> step{0};
    lv_obj_t* image = nullptr;
    lv_obj_t* label = nullptr;
    int max_width = 0;
    int max_height = 0;
    // Only touched with the LVGL lock held
    std::unique_ptr<Rgb565Image> shown;
};

class PhotoApp : public DesktopApp {
public:
    explicit PhotoApp(const DesktopServices& services) : services_(services) {}

    void Open(lv_obj_t* screen) override {
        state_ = std::make_shared<PhotoState>();
        if (!services_.ensure_tf_mounted()) {
            CreateNotice(screen, services_.text_font, "No TF card");
            return;
        }
        state_->files = ListMediaFiles(kPictureDir, {"jpg", "jpeg", "png", "bmp"});
        if (state_->files.empty()) {
            CreateNotice(screen, services_.text_font, "No pictures in /sdcard/PICTURE");
            return;
        }
        state_->max_width = lv_obj_get_width(screen);
        state_->max_height = lv_obj_get_height(screen);

        state_->image = lv_image_create(screen);
        lv_obj_center(state_->image);

        auto prev = CreateIconButton(screen, &desktop_img_prev, OnStep, this);
        lv_obj_align(prev, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_set_user_data(prev, reinterpret_cast<void*>(-1));
        auto next = CreateIconButton(screen, &desktop_img_next, OnStep, this);
        lv_obj_align(next, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_user_data(next, reinterpret_cast<void*>(1));

        state_->label = lv_label_create(screen);
        lv_obj_set_style_text_color(state_->label, lv_color_white(), 0);
        lv_obj_set_style_text_font(state_->label, services_.text_font, 0);
        lv_obj_set_style_bg_color(state_->label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(state_->label, LV_OPA_50, 0);
        lv_obj_set_style_pad_hor(state_->label, 6, 0);
        lv_label_set_long_mode(state_->label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(state_->label, LV_PCT(70));
        lv_obj_align(state_->label, LV_ALIGN_BOTTOM_MID, 0, -4);

        lv_obj_add_event_cb(screen, OnGesture, LV_EVENT_GESTURE, this);

        auto state = state_;
        StartWorkerTask("photo", 8192, 3, [state]() { Run(state); });
    }

    void Close() override {
        if (state_) {
            state_->session.Cancel();
        }
    }

private:
    const DesktopServices& services_;
    std::shared_ptr<PhotoState> state_;

    static void OnStep(lv_event_t* e) {
        auto self = static_cast<PhotoApp*>(lv_event_get_user_data(e));
        auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        self->state_->step =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    }

    static void OnGesture(lv_event_t* e) {
        auto self = static_cast<PhotoApp*>(lv_event_get_user_data(e));
        auto dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir == LV_DIR_LEFT) {
            self->state_->step = 1;
        } else if (dir == LV_DIR_RIGHT) {
            self->state_->step = -1;
        }
    }

    static void Run(std::shared_ptr<PhotoState> state) {
        int count = static_cast<int>(state->files.size());
        int index = 0;
        while (true) {
            const std::string& path = state->files[index];
            std::string error;
            std::unique_ptr<Rgb565Image> decoded;
            if (IsPngFile(path)) {
                // lodepng allocates from the LVGL heap
                if (!state->session.RunLocked([&]() {
                        decoded = DecodeImageFile(path, state->max_width, state->max_height, error);
                    })) {
                    break;
                }
            } else {
                decoded = DecodeImageFile(path, state->max_width, state->max_height, error);
            }

            std::unique_ptr<Rgb565Image> previous;
            bool shown = state->session.RunLocked([&]() {
                if (decoded) {
                    lv_image_cache_drop(decoded->dsc());
                    lv_image_set_src(state->image, decoded->dsc());
                    previous = std::move(state->shown);
                    if (previous) {
                        lv_image_cache_drop(previous->dsc());
                    }
                    state->shown = std::move(decoded);
                    lv_label_set_text_fmt(state->label, "%d/%d  %s", index + 1, count,
                                          BaseName(path).c_str());
                } else {
                    lv_label_set_text_fmt(state->label, "%s: %s", BaseName(path).c_str(),
                                          error.c_str());
                }
            });
            // The previous buffer is no longer referenced once the new source is set
            previous.reset();
            if (!shown) {
                break;
            }
            if (!error.empty()) {
                ESP_LOGW(TAG, "%s: %s", path.c_str(), error.c_str());
            }

            int step = 0;
            while ((step = state->step.exchange(0)) == 0) {
                if (!state->session.Sleep(30)) {
                    return;
                }
            }
            index = ((index + step) % count + count) % count;
        }
    }
};

}  // namespace

std::unique_ptr<DesktopApp> CreatePhotoApp(const DesktopServices& services) {
    return std::make_unique<PhotoApp>(services);
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
