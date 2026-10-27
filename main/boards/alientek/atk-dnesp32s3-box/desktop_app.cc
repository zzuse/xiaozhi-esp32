#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "desktop_app.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <dirent.h>
#include <freertos/task.h>
#include <strings.h>

#include <algorithm>

#define TAG "DesktopApp"

static constexpr int kLockPollMs = 20;

bool UiSession::RunLocked(const std::function<void()>& fn) {
    while (!cancelled_) {
        if (!lvgl_port_lock(kLockPollMs)) {
            continue;
        }
        // Close() cancels while holding the lock, so this check cannot race with it
        bool run = !cancelled_;
        if (run) {
            fn();
        }
        lvgl_port_unlock();
        return run;
    }
    return false;
}

bool UiSession::Sleep(int ms) {
    while (ms > 0 && !cancelled_) {
        int step = std::min(ms, kLockPollMs);
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
    return !cancelled_;
}

bool StartWorkerTask(const char* name, uint32_t stack_size, UBaseType_t priority,
                     std::function<void()> fn) {
    auto heap_fn = new std::function<void()>(std::move(fn));
    BaseType_t ret = xTaskCreate(
        [](void* arg) {
            auto task_fn = static_cast<std::function<void()>*>(arg);
            (*task_fn)();
            delete task_fn;
            vTaskDelete(nullptr);
        },
        name, stack_size, heap_fn, priority, nullptr);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to start task %s", name);
        delete heap_fn;
        return false;
    }
    return true;
}

bool HasExtension(const std::string& path, const char* ext) {
    size_t ext_len = strlen(ext);
    if (path.size() <= ext_len || path[path.size() - ext_len - 1] != '.') {
        return false;
    }
    return strcasecmp(path.c_str() + path.size() - ext_len, ext) == 0;
}

std::string BaseName(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::vector<std::string> ListMediaFiles(const char* dir, std::initializer_list<const char*> exts) {
    std::vector<std::string> files;
    DIR* d = opendir(dir);
    if (d == nullptr) {
        return files;
    }
    while (auto entry = readdir(d)) {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.') {
            continue;
        }
        std::string name = entry->d_name;
        for (auto ext : exts) {
            if (HasExtension(name, ext)) {
                files.push_back(std::string(dir) + "/" + name);
                break;
            }
        }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

lv_obj_t* CreateNotice(lv_obj_t* parent, const lv_font_t* font, const char* text) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(label, LV_PCT(80));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return label;
}

lv_obj_t* CreateIconButton(lv_obj_t* parent, const void* image_or_symbol, lv_event_cb_t cb,
                           void* user_data) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 48, 48);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    if (lv_image_src_get_type(image_or_symbol) == LV_IMAGE_SRC_SYMBOL) {
        lv_obj_t* label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, static_cast<const char*>(image_or_symbol));
        lv_obj_center(label);
    } else {
        lv_obj_t* image = lv_image_create(button);
        lv_image_set_src(image, image_or_symbol);
        lv_obj_center(image);
    }
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    return button;
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
