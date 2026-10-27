#ifndef DESKTOP_DISPLAY_H
#define DESKTOP_DISPLAY_H

#include "desktop_app.h"
#include "lcd_display.h"

// LCD display with a launcher desktop. The regular Xiaozhi chat UI becomes one screen
// ("Chat"), next to the Camera, Video, Photo and Music app screens.
//
// Whenever a conversation is running (or the device is still starting, configuring or
// upgrading) the chat screen is shown, closing any open app.
class DesktopDisplay : public SpiLcdDisplay {
public:
    DesktopDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy, DesktopServices services);

    virtual void SetupUI() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

private:
    enum class AppId { kChat, kCamera, kVideo, kPhoto, kMusic };

    DesktopServices services_;
    lv_obj_t* chat_screen_ = nullptr;
    lv_obj_t* desktop_screen_ = nullptr;
    lv_obj_t* app_screen_ = nullptr;
    lv_obj_t* home_button_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* status_time_label_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* tf_icon_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::unique_ptr<DesktopApp> app_;

    bool reached_idle_ = false;
    bool chat_opened_automatically_ = false;
    int idle_ticks_ = 0;
    int status_ticks_ = 0;

    void BuildDesktop();
    void BuildHomeButton();
    void AddAppIcon(lv_obj_t* parent, const lv_image_dsc_t* icon, const char* name, AppId id);
    void OpenApp(AppId id);
    void CloseApp();
    void ShowDesktop();
    void ShowChat(bool automatic);
    void OnTimer();
    void UpdateStatus();

    static void OnAppIconClicked(lv_event_t* e);
    static void OnHomeButtonEvent(lv_event_t* e);
};

#endif  // DESKTOP_DISPLAY_H
