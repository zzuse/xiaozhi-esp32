#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "desktop_app.h"
#include "media_audio.h"
#include "media_image.h"

#define TAG "VideoApp"

LV_IMAGE_DECLARE(desktop_img_prev);
LV_IMAGE_DECLARE(desktop_img_next);

static constexpr const char* kVideoDir = "/sdcard/VIDEO";
static constexpr uint32_t kMaxHeaderListSize = 64 * 1024;
static constexpr uint32_t kMaxChunkSize = 512 * 1024;
static constexpr uint16_t kWaveFormatPcm = 1;

namespace {

constexpr uint32_t FourCC(const char (&s)[5]) {
    return s[0] | (s[1] << 8) | (s[2] << 16) | (static_cast<uint32_t>(s[3]) << 24);
}

uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
uint16_t Le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

struct AviInfo {
    uint32_t us_per_frame = 0;
    uint32_t total_frames = 0;
    int video_stream = -1;
    int audio_stream = -1;
    uint16_t audio_format = 0;
    int audio_channels = 0;
    int audio_sample_rate = 0;
    int audio_bits = 0;
    long movi_start = 0;
    long movi_end = 0;
};

// Parses the hdrl list: the main header plus one strl list per stream
void ParseHeaderList(const uint8_t* buf, uint32_t len, AviInfo& info) {
    int stream = 0;
    uint32_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t id = Le32(buf + pos);
        uint32_t size = Le32(buf + pos + 4);
        const uint8_t* data = buf + pos + 8;
        if (pos + 8 + size > len) {
            break;
        }
        if (id == FourCC("avih") && size >= 40) {
            info.us_per_frame = Le32(data);
            info.total_frames = Le32(data + 16);
        } else if (id == FourCC("LIST") && size >= 4 && Le32(data) == FourCC("strl")) {
            uint32_t type = 0;
            uint32_t sub = 4;
            while (sub + 8 <= size) {
                uint32_t sub_id = Le32(data + sub);
                uint32_t sub_size = Le32(data + sub + 4);
                const uint8_t* sub_data = data + sub + 8;
                if (sub + 8 + sub_size > size) {
                    break;
                }
                if (sub_id == FourCC("strh") && sub_size >= 8) {
                    type = Le32(sub_data);
                } else if (sub_id == FourCC("strf")) {
                    if (type == FourCC("vids") && sub_size >= 20 && info.video_stream < 0 &&
                        Le32(sub_data + 16) == FourCC("MJPG")) {
                        info.video_stream = stream;
                    } else if (type == FourCC("auds") && sub_size >= 16 && info.audio_stream < 0) {
                        info.audio_stream = stream;
                        info.audio_format = Le16(sub_data);
                        info.audio_channels = Le16(sub_data + 2);
                        info.audio_sample_rate = static_cast<int>(Le32(sub_data + 4));
                        info.audio_bits = Le16(sub_data + 14);
                    }
                }
                sub += 8 + sub_size + (sub_size & 1);
            }
            stream++;
        }
        pos += 8 + size + (size & 1);
    }
}

bool ParseAvi(FILE* f, AviInfo& info, std::string& error) {
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) || Le32(header) != FourCC("RIFF") ||
        Le32(header + 8) != FourCC("AVI ")) {
        error = "Not an AVI file";
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    long pos = 12;
    while (pos + 12 <= file_size) {
        uint8_t chunk[12];
        fseek(f, pos, SEEK_SET);
        if (fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk)) {
            break;
        }
        uint32_t id = Le32(chunk);
        uint32_t size = Le32(chunk + 4);
        if (id == FourCC("LIST")) {
            uint32_t list_type = Le32(chunk + 8);
            if (list_type == FourCC("hdrl") && size > 4 && size <= kMaxHeaderListSize) {
                auto buf = static_cast<uint8_t*>(heap_caps_malloc(size - 4, MALLOC_CAP_SPIRAM));
                if (buf == nullptr) {
                    error = "Out of memory";
                    return false;
                }
                size_t read = fread(buf, 1, size - 4, f);
                ParseHeaderList(buf, read, info);
                heap_caps_free(buf);
            } else if (list_type == FourCC("movi")) {
                info.movi_start = pos + 12;
                info.movi_end = std::min<long>(pos + 8 + size, file_size);
                break;
            }
        }
        pos += 8 + size + (size & 1);
    }
    if (info.video_stream < 0) {
        error = "Only MJPEG AVI is supported";
        return false;
    }
    if (info.movi_start == 0) {
        error = "AVI has no movie data";
        return false;
    }
    if (info.us_per_frame == 0) {
        info.us_per_frame = 40000;
    }
    return true;
}

// Chunk ids are "NNdc"/"NNdb" for video and "NNwb" for audio, NN being the stream number
int ChunkStream(uint32_t id) {
    int hi = static_cast<int>(id & 0xFF) - '0';
    int lo = static_cast<int>((id >> 8) & 0xFF) - '0';
    if (hi < 0 || hi > 9 || lo < 0 || lo > 9) {
        return -1;
    }
    return hi * 10 + lo;
}

struct VideoState {
    UiSession session;
    std::vector<std::string> files;
    std::atomic<int> step{0};
    std::atomic<bool> paused{false};
    lv_obj_t* image = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* time = nullptr;
    lv_obj_t* play_label = nullptr;
    lv_obj_t* controls = nullptr;
    int max_width = 0;
    int max_height = 0;
    // Double buffer: decode into frames[back] while LVGL shows the other one
    Rgb565Image frames[2];
    int back = 0;
    JpegDecoder decoder;
};

class VideoApp : public DesktopApp {
public:
    explicit VideoApp(const DesktopServices& services) : services_(services) {}

    void Open(lv_obj_t* screen) override {
        state_ = std::make_shared<VideoState>();
        if (!services_.ensure_tf_mounted()) {
            CreateNotice(screen, services_.text_font, "No TF card");
            return;
        }
        state_->files = ListMediaFiles(kVideoDir, {"avi"});
        if (state_->files.empty()) {
            CreateNotice(screen, services_.text_font, "No AVI (MJPEG) videos in /sdcard/VIDEO");
            return;
        }
        state_->max_width = lv_obj_get_width(screen);
        state_->max_height = lv_obj_get_height(screen);

        state_->image = lv_image_create(screen);
        lv_obj_center(state_->image);
        lv_obj_add_flag(state_->image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(state_->image, OnToggleControls, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(screen, OnToggleControls, LV_EVENT_CLICKED, this);

        // Bottom control bar: title, time and transport buttons
        auto bar = lv_obj_create(screen);
        state_->controls = bar;
        lv_obj_set_size(bar, LV_PCT(100), 64);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 4, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

        auto prev = CreateIconButton(bar, &desktop_img_prev, OnStep, this);
        lv_obj_set_user_data(prev, reinterpret_cast<void*>(-1));
        lv_obj_align(prev, LV_ALIGN_LEFT_MID, 0, 0);
        auto play = CreateIconButton(bar, LV_SYMBOL_PAUSE, OnPlayPause, this);
        state_->play_label = lv_obj_get_child(play, 0);
        lv_obj_align_to(play, prev, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        auto next = CreateIconButton(bar, &desktop_img_next, OnStep, this);
        lv_obj_set_user_data(next, reinterpret_cast<void*>(1));
        lv_obj_align_to(next, play, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

        state_->title = lv_label_create(bar);
        lv_obj_set_style_text_color(state_->title, lv_color_white(), 0);
        lv_obj_set_style_text_font(state_->title, services_.text_font, 0);
        lv_label_set_long_mode(state_->title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(state_->title, 140);
        lv_obj_align(state_->title, LV_ALIGN_TOP_RIGHT, 0, 0);
        state_->time = lv_label_create(bar);
        lv_obj_set_style_text_color(state_->time, lv_color_white(), 0);
        lv_obj_set_style_text_font(state_->time, &lv_font_montserrat_14, 0);
        lv_label_set_text(state_->time, "");
        lv_obj_align(state_->time, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        auto state = state_;
        auto services = &services_;
        StartWorkerTask("video", 8192, 4, [state, services]() { Run(state, *services); });
    }

    void Close() override {
        if (state_) {
            state_->session.Cancel();
        }
    }

private:
    const DesktopServices& services_;
    std::shared_ptr<VideoState> state_;

    static void OnStep(lv_event_t* e) {
        auto self = static_cast<VideoApp*>(lv_event_get_user_data(e));
        auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        self->state_->step =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    }

    static void OnPlayPause(lv_event_t* e) {
        auto self = static_cast<VideoApp*>(lv_event_get_user_data(e));
        bool paused = !self->state_->paused;
        self->state_->paused = paused;
        lv_label_set_text(self->state_->play_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }

    static void OnToggleControls(lv_event_t* e) {
        auto self = static_cast<VideoApp*>(lv_event_get_user_data(e));
        auto controls = self->state_->controls;
        if (lv_obj_has_flag(controls, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(controls, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(controls, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static void Run(std::shared_ptr<VideoState> state, const DesktopServices& services) {
        MediaAudioOutput audio(services);
        uint8_t* chunk = static_cast<uint8_t*>(heap_caps_malloc(kMaxChunkSize, MALLOC_CAP_SPIRAM));
        if (chunk == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate chunk buffer");
            return;
        }
        int count = static_cast<int>(state->files.size());
        int index = 0;
        while (!state->session.cancelled()) {
            bool finished = PlayFile(state, audio, chunk, state->files[index]);
            int step = state->step.exchange(0);
            if (step == 0) {
                step = 1;
            }
            audio.End(finished);
            index = ((index + step) % count + count) % count;
        }
        audio.End(false);
        heap_caps_free(chunk);
    }

    // Returns true when the file played to the end
    static bool PlayFile(std::shared_ptr<VideoState>& state, MediaAudioOutput& audio,
                         uint8_t* chunk, const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        AviInfo info;
        std::string error;
        bool parsed = ParseAvi(f, info, error);
        uint32_t total_sec = static_cast<uint32_t>(static_cast<uint64_t>(info.total_frames) *
                                                   info.us_per_frame / 1000000);
        if (!state->session.RunLocked([&]() {
                lv_label_set_text(state->title, BaseName(path).c_str());
                if (!parsed) {
                    lv_label_set_text(state->time, error.c_str());
                }
            }) ||
            !parsed) {
            fclose(f);
            state->session.Sleep(1500);
            return false;
        }

        bool audio_ok = info.audio_stream >= 0 && info.audio_format == kWaveFormatPcm &&
                        info.audio_bits == 16 &&
                        audio.Begin(info.audio_sample_rate, info.audio_channels);
        if (info.audio_stream >= 0 && !audio_ok) {
            ESP_LOGW(TAG, "%s: unsupported audio (format %u, %d bits), playing muted", path.c_str(),
                     info.audio_format, info.audio_bits);
        }
        auto keep_going = [&state]() { return !state->session.cancelled() && state->step == 0; };

        int64_t start_us = esp_timer_get_time();
        uint32_t frame_no = 0;
        uint32_t shown_sec = UINT32_MAX;
        int& back = state->back;
        long pos = info.movi_start;
        bool finished = false;
        fseek(f, pos, SEEK_SET);
        while (keep_going()) {
            if (state->paused) {
                int64_t pause_start = esp_timer_get_time();
                while (state->paused && keep_going()) {
                    state->session.Sleep(50);
                }
                start_us += esp_timer_get_time() - pause_start;
                continue;
            }
            if (pos + 8 > info.movi_end) {
                finished = true;
                break;
            }
            uint8_t header[8];
            if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
                finished = true;
                break;
            }
            uint32_t id = Le32(header);
            uint32_t size = Le32(header + 4);
            if (id == FourCC("LIST")) {
                // "rec " lists group interleaved chunks; descend into them
                fseek(f, 4, SEEK_CUR);
                pos += 12;
                continue;
            }
            long next = pos + 8 + size + (size & 1);
            int stream = ChunkStream(id);
            uint32_t kind = id >> 16;
            bool is_video = stream == info.video_stream &&
                            (kind == ('d' | ('c' << 8)) || kind == ('d' | ('b' << 8)));
            bool is_audio = audio_ok && stream == info.audio_stream && kind == ('w' | ('b' << 8));

            if (is_video && size > 0 && size <= kMaxChunkSize) {
                int64_t due = start_us + static_cast<int64_t>(frame_no) * info.us_per_frame;
                frame_no++;
                // Drop frames instead of letting the picture lag behind the sound
                if (esp_timer_get_time() <= due + 2 * static_cast<int64_t>(info.us_per_frame) &&
                    fread(chunk, 1, size, f) == size &&
                    state->decoder.Decode(chunk, size, state->max_width, state->max_height,
                                          state->frames[back])) {
                    int64_t wait_ms = (due - esp_timer_get_time()) / 1000;
                    if (wait_ms > 0 && !state->session.Sleep(static_cast<int>(wait_ms))) {
                        break;
                    }
                    uint32_t sec = static_cast<uint32_t>(static_cast<uint64_t>(frame_no) *
                                                         info.us_per_frame / 1000000);
                    auto& frame = state->frames[back];
                    if (!state->session.RunLocked([&]() {
                            lv_image_cache_drop(frame.dsc());
                            lv_image_set_src(state->image, frame.dsc());
                            if (sec != shown_sec) {
                                lv_label_set_text_fmt(state->time, "%02u:%02u / %02u:%02u",
                                                      unsigned(sec / 60), unsigned(sec % 60),
                                                      unsigned(total_sec / 60),
                                                      unsigned(total_sec % 60));
                            }
                        })) {
                        break;
                    }
                    shown_sec = sec;
                    back ^= 1;
                }
            } else if (is_audio && size > 0 && size <= kMaxChunkSize) {
                if (fread(chunk, 1, size, f) == size) {
                    size_t frames = size / (2 * info.audio_channels);
                    audio.Write(reinterpret_cast<const int16_t*>(chunk), frames, keep_going);
                }
            }
            pos = next;
            fseek(f, pos, SEEK_SET);
        }
        fclose(f);
        return finished;
    }
};

}  // namespace

std::unique_ptr<DesktopApp> CreateVideoApp(const DesktopServices& services) {
    return std::make_unique<VideoApp>(services);
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
