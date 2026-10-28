#ifndef DESKTOP_APP_H
#define DESKTOP_APP_H

#include <freertos/FreeRTOS.h>
#include <lvgl.h>
#include <src/misc/cache/lv_cache.h>

#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

class AudioCodec;
class LvglImage;

// Board services the desktop apps depend on.
struct DesktopServices {
    std::function<AudioCodec*()> audio_codec;
    // Keeps the codec output powered while media plays outside AudioService.
    std::function<void(bool)> set_media_output_hold;
    std::function<bool()> is_tf_mounted;
    // Mounts the TF card if needed (slow when no card is inserted). LVGL task only.
    std::function<bool()> ensure_tf_mounted;
    // Theme text font, refreshed each time an app opens. Do not keep it past Close():
    // the theme can swap fonts at runtime and free the old one.
    const lv_font_t* text_font = nullptr;
};

// Cancellation token shared by an app and its worker task.
//
// Apps are opened and closed in the LVGL task with the LVGL lock held. Close() only
// cancels the session; the worker notices on its next RunLocked() and exits on its own,
// so the LVGL task never waits on a worker that might itself be waiting for the lock.
class UiSession {
public:
    void Cancel() { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }

    // Runs fn with the LVGL lock held unless the session was cancelled.
    // Returns false when cancelled; UI objects of a cancelled session may be gone.
    bool RunLocked(const std::function<void()>& fn);

    // Sleeps in small steps; returns false when cancelled meanwhile.
    bool Sleep(int ms);

private:
    std::atomic<bool> cancelled_{false};
};

class DesktopApp {
public:
    virtual ~DesktopApp() = default;
    // Builds the app UI inside screen. Runs in the LVGL task with the lock held.
    virtual void Open(lv_obj_t* screen) = 0;
    // Stops background work without blocking. Runs in the LVGL task with the lock held,
    // right before the app screen is deleted.
    virtual void Close() = 0;
    // Camera preview frames are offered to the active app before the chat UI.
    virtual bool ConsumePreviewImage(std::unique_ptr<LvglImage>& image) { return false; }
};

std::unique_ptr<DesktopApp> CreateCameraApp(const DesktopServices& services);
std::unique_ptr<DesktopApp> CreateVideoApp(const DesktopServices& services);
std::unique_ptr<DesktopApp> CreatePhotoApp(const DesktopServices& services);
std::unique_ptr<DesktopApp> CreateMusicApp(const DesktopServices& services);

// Helpers shared by the apps
bool StartWorkerTask(const char* name, uint32_t stack_size, UBaseType_t priority,
                     std::function<void()> fn);
// Sorted full paths of the files in dir whose extension (case-insensitive) is listed.
std::vector<std::string> ListMediaFiles(const char* dir, std::initializer_list<const char*> exts);
std::string BaseName(const std::string& path);
bool HasExtension(const std::string& path, const char* ext);

lv_obj_t* CreateNotice(lv_obj_t* parent, const lv_font_t* font, const char* text);
lv_obj_t* CreateIconButton(lv_obj_t* parent, const void* image_or_symbol, lv_event_cb_t cb,
                           void* user_data);

#endif  // DESKTOP_APP_H
