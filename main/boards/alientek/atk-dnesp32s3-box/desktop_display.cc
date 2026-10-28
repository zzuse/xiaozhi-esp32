#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "desktop_display.h"

#include <esp_log.h>
#include <esp_wifi.h>

#include <cstdlib>
#include <ctime>

#include "application.h"
#include "device_state.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"

#define TAG "DesktopDisplay"

LV_IMAGE_DECLARE(desktop_img_wallpaper);
LV_IMAGE_DECLARE(desktop_img_chat);
LV_IMAGE_DECLARE(desktop_img_camera);
LV_IMAGE_DECLARE(desktop_img_video);
LV_IMAGE_DECLARE(desktop_img_photo);
LV_IMAGE_DECLARE(desktop_img_music);
LV_IMAGE_DECLARE(desktop_img_wifi);
LV_IMAGE_DECLARE(desktop_img_tf);

static constexpr uint32_t kTimerPeriodMs = 300;
static constexpr int kStatusEveryTicks = 3;
// Return to the desktop this long after a conversation that interrupted it ends
static constexpr int kAutoReturnTicks = 4000 / kTimerPeriodMs;
static constexpr int kStatusBarHeight = 20;
static constexpr int kIconSize = 48;
static constexpr int kHomeButtonSize = 44;
static constexpr int kDragThreshold = 6;

static bool home_button_dragged = false;

DesktopDisplay::DesktopDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy, DesktopServices services)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy),
      services_(std::move(services)) {}

void DesktopDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    if (desktop_screen_ != nullptr) {
        return;
    }
    chat_screen_ = lv_screen_active();

    BuildDesktop();
    BuildHomeButton();
    UpdateStatus();
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<DesktopDisplay*>(lv_timer_get_user_data(timer))->OnTimer();
        },
        kTimerPeriodMs, this);
}

void DesktopDisplay::BuildDesktop() {
    desktop_screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(desktop_screen_, lv_color_black(), 0);
    lv_obj_remove_flag(desktop_screen_, LV_OBJ_FLAG_SCROLLABLE);

    auto wallpaper = lv_image_create(desktop_screen_);
    lv_image_set_src(wallpaper, &desktop_img_wallpaper);
    lv_obj_set_pos(wallpaper, 0, kStatusBarHeight);

    // Status bar: connectivity on the left, time in the middle
    auto bar = lv_obj_create(desktop_screen_);
    lv_obj_set_size(bar, LV_PCT(100), kStatusBarHeight);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    wifi_icon_ = lv_image_create(bar);
    lv_image_set_src(wifi_icon_, &desktop_img_wifi);
    lv_obj_align(wifi_icon_, LV_ALIGN_LEFT_MID, 0, 0);
    tf_icon_ = lv_image_create(bar);
    lv_image_set_src(tf_icon_, &desktop_img_tf);
    lv_obj_align_to(tf_icon_, wifi_icon_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    status_time_label_ = lv_label_create(bar);
    lv_obj_set_style_text_font(status_time_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(status_time_label_, lv_color_white(), 0);
    lv_label_set_text(status_time_label_, "");
    lv_obj_center(status_time_label_);

    clock_label_ = lv_label_create(desktop_screen_);
    lv_obj_set_style_text_font(clock_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
    lv_label_set_text(clock_label_, "--:--");
    lv_obj_set_pos(clock_label_, 20, kStatusBarHeight + 10);

    date_label_ = lv_label_create(desktop_screen_);
    lv_obj_set_style_text_font(date_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_hex(0xC8D8E8), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align_to(date_label_, clock_label_, LV_ALIGN_OUT_BOTTOM_LEFT, 4, 0);

    auto dock = lv_obj_create(desktop_screen_);
    lv_obj_set_size(dock, LV_PCT(100), 84);
    lv_obj_align(dock, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dock, 0, 0);
    lv_obj_set_style_pad_all(dock, 0, 0);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    AddAppIcon(dock, &desktop_img_chat, "Chat", AppId::kChat);
    AddAppIcon(dock, &desktop_img_camera, "Camera", AppId::kCamera);
    AddAppIcon(dock, &desktop_img_video, "Video", AppId::kVideo);
    AddAppIcon(dock, &desktop_img_photo, "Photos", AppId::kPhoto);
    AddAppIcon(dock, &desktop_img_music, "Music", AppId::kMusic);
}

void DesktopDisplay::AddAppIcon(lv_obj_t* parent, const lv_image_dsc_t* icon, const char* name,
                                AppId id) {
    auto item = lv_obj_create(parent);
    lv_obj_set_size(item, 60, 76);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_opa(item, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(item, 4, 0);

    auto image = lv_image_create(item);
    lv_image_set_src(image, icon);
    lv_obj_set_size(image, kIconSize, kIconSize);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(image, LV_SCALE_NONE * kIconSize / icon->header.w);

    auto label = lv_label_create(item);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, name);

    lv_obj_set_user_data(item, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
    lv_obj_add_event_cb(item, OnAppIconClicked, LV_EVENT_CLICKED, this);
}

void DesktopDisplay::BuildHomeButton() {
    // Lives on the top layer so it floats above every screen; it can be dragged aside
    home_button_ = lv_button_create(lv_layer_top());
    lv_obj_set_size(home_button_, kHomeButtonSize, kHomeButtonSize);
    lv_obj_set_pos(home_button_, width_ - kHomeButtonSize - 8, kStatusBarHeight + 8);
    lv_obj_set_style_radius(home_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(home_button_, LV_OPA_50, 0);
    lv_obj_set_style_outline_width(home_button_, 2, 0);
    lv_obj_set_style_outline_color(home_button_, lv_color_hex(0x22B14C), 0);
    lv_obj_set_style_outline_opa(home_button_, LV_OPA_50, 0);
    lv_obj_set_style_shadow_width(home_button_, 0, 0);

    auto label = lv_label_create(home_button_);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x303030), 0);
    lv_label_set_text(label, LV_SYMBOL_HOME);
    lv_obj_center(label);

    lv_obj_add_event_cb(home_button_, OnHomeButtonEvent, LV_EVENT_ALL, this);
    lv_obj_add_flag(home_button_, LV_OBJ_FLAG_HIDDEN);
}

void DesktopDisplay::OnHomeButtonEvent(lv_event_t* e) {
    auto self = static_cast<DesktopDisplay*>(lv_event_get_user_data(e));
    auto code = lv_event_get_code(e);
    auto button = self->home_button_;
    if (code == LV_EVENT_PRESSED) {
        home_button_dragged = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t vect;
        lv_indev_get_vect(lv_indev_active(), &vect);
        if (vect.x == 0 && vect.y == 0) {
            return;
        }
        int x = lv_obj_get_x(button) + vect.x;
        int y = lv_obj_get_y(button) + vect.y;
        x = LV_CLAMP(0, x, self->width_ - kHomeButtonSize);
        y = LV_CLAMP(0, y, self->height_ - kHomeButtonSize);
        lv_obj_set_pos(button, x, y);
        if (std::abs(vect.x) + std::abs(vect.y) >= kDragThreshold) {
            home_button_dragged = true;
        }
    } else if (code == LV_EVENT_CLICKED && !home_button_dragged) {
        self->ShowDesktop();
    }
}

void DesktopDisplay::OnAppIconClicked(lv_event_t* e) {
    auto self = static_cast<DesktopDisplay*>(lv_event_get_user_data(e));
    auto item = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    auto id = static_cast<AppId>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(item)));
    self->OpenApp(id);
}

void DesktopDisplay::OpenApp(AppId id) {
    if (id == AppId::kChat) {
        ShowChat(false);
        return;
    }
    auto screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(screen);
    CloseApp();

    // Read the font now: loading the assets partition replaces the theme font after
    // SetupUI and frees the previous one, so a pointer cached earlier would dangle.
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    services_.text_font = lvgl_theme->text_font()->font();

    app_screen_ = screen;
    switch (id) {
        case AppId::kCamera:
            app_ = CreateCameraApp(services_);
            break;
        case AppId::kVideo:
            app_ = CreateVideoApp(services_);
            break;
        case AppId::kPhoto:
            app_ = CreatePhotoApp(services_);
            break;
        case AppId::kMusic:
            app_ = CreateMusicApp(services_);
            break;
        default:
            break;
    }
    if (app_) {
        app_->Open(screen);
    }
    lv_obj_remove_flag(home_button_, LV_OBJ_FLAG_HIDDEN);
}

void DesktopDisplay::CloseApp() {
    if (app_) {
        app_->Close();
    }
    if (app_screen_ != nullptr) {
        lv_obj_delete(app_screen_);
        app_screen_ = nullptr;
    }
    app_.reset();
    // App images live in buffers that are freed with the app
    lv_image_cache_drop(nullptr);
}

void DesktopDisplay::ShowDesktop() {
    chat_opened_automatically_ = false;
    idle_ticks_ = 0;
    if (lv_screen_active() != desktop_screen_) {
        lv_screen_load(desktop_screen_);
    }
    CloseApp();
    lv_obj_add_flag(home_button_, LV_OBJ_FLAG_HIDDEN);
    UpdateStatus();
}

void DesktopDisplay::ShowChat(bool automatic) {
    idle_ticks_ = 0;
    if (lv_screen_active() == chat_screen_) {
        return;
    }
    chat_opened_automatically_ = automatic;
    lv_screen_load(chat_screen_);
    CloseApp();
    lv_obj_remove_flag(home_button_, LV_OBJ_FLAG_HIDDEN);
}

void DesktopDisplay::OnTimer() {
    auto state = Application::GetInstance().GetDeviceState();
    switch (state) {
        case kDeviceStateIdle:
            if (!reached_idle_) {
                // Boot finished: leave the setup messages for the desktop
                reached_idle_ = true;
                ShowDesktop();
            } else if (chat_opened_automatically_ && lv_screen_active() == chat_screen_ &&
                       ++idle_ticks_ >= kAutoReturnTicks) {
                ShowDesktop();
            }
            break;
        case kDeviceStateUnknown:
            break;
        default:
            // Conversations, setup, activation and upgrades all need the chat screen
            ShowChat(true);
            break;
    }

    if (++status_ticks_ >= kStatusEveryTicks) {
        status_ticks_ = 0;
        if (lv_screen_active() == desktop_screen_) {
            UpdateStatus();
        }
    }
}

void DesktopDisplay::UpdateStatus() {
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year >= 2025 - 1900) {
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M", &tm_now);
        lv_label_set_text(clock_label_, buf);
        lv_label_set_text(status_time_label_, buf);
        strftime(buf, sizeof(buf), "%a, %b %d", &tm_now);
        lv_label_set_text(date_label_, buf);
    }

    wifi_ap_record_t ap_info;
    bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    lv_obj_set_style_image_opa(wifi_icon_, wifi_connected ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_image_opa(tf_icon_, services_.is_tf_mounted() ? LV_OPA_COVER : LV_OPA_40, 0);
}

void DesktopDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    {
        DisplayLockGuard lock(this);
        if (app_ != nullptr && lv_screen_active() == app_screen_ &&
            app_->ConsumePreviewImage(image)) {
            return;
        }
    }
    SpiLcdDisplay::SetPreviewImage(std::move(image));
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
