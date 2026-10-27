#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>
#include <mutex>

#include "audio_codec.h"
#include "desktop_app.h"
#include "media_audio.h"

#define TAG "MusicApp"

LV_IMAGE_DECLARE(desktop_img_album);
LV_IMAGE_DECLARE(desktop_img_prev);
LV_IMAGE_DECLARE(desktop_img_next);

static constexpr const char* kMusicDir = "/sdcard/MUSIC";
static constexpr size_t kInputChunk = 4096;
static constexpr size_t kInitialOutputSize = 8192;
static constexpr int kProgressIntervalMs = 500;

namespace {

esp_audio_simple_dec_type_t DecoderTypeFor(const std::string& path) {
    if (HasExtension(path, "mp3"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (HasExtension(path, "wav"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (HasExtension(path, "aac"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (HasExtension(path, "m4a"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (HasExtension(path, "flac"))
        return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

void RegisterDecoders() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
}

// Reduce 24/32-bit samples to 16-bit in place
size_t To16Bit(uint8_t* data, size_t bytes, int bits) {
    if (bits == 16) {
        return bytes / 2;
    }
    int width = bits / 8;
    size_t samples = bytes / width;
    auto out = reinterpret_cast<int16_t*>(data);
    for (size_t i = 0; i < samples; i++) {
        const uint8_t* p = data + i * width;
        out[i] = static_cast<int16_t>(p[width - 2] | (p[width - 1] << 8));
    }
    return samples;
}

struct MusicState {
    UiSession session;
    std::vector<std::string> files;
    std::atomic<int> step{0};
    std::atomic<bool> paused{false};
    lv_obj_t* title = nullptr;
    lv_obj_t* index = nullptr;
    lv_obj_t* time = nullptr;
    lv_obj_t* progress = nullptr;
    lv_obj_t* play_label = nullptr;
};

class MusicApp : public DesktopApp {
public:
    explicit MusicApp(const DesktopServices& services) : services_(services) {}

    void Open(lv_obj_t* screen) override {
        state_ = std::make_shared<MusicState>();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x1C1C2E), 0);
        if (!services_.ensure_tf_mounted()) {
            CreateNotice(screen, services_.text_font, "No TF card");
            return;
        }
        state_->files = ListMediaFiles(kMusicDir, {"mp3", "wav", "aac", "m4a", "flac"});
        if (state_->files.empty()) {
            CreateNotice(screen, services_.text_font, "No music in /sdcard/MUSIC");
            return;
        }

        auto album = lv_image_create(screen);
        lv_image_set_src(album, &desktop_img_album);
        lv_obj_align(album, LV_ALIGN_TOP_LEFT, 16, 20);

        state_->title = lv_label_create(screen);
        lv_obj_set_style_text_color(state_->title, lv_color_white(), 0);
        lv_obj_set_style_text_font(state_->title, services_.text_font, 0);
        lv_label_set_long_mode(state_->title, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(state_->title, 124);
        lv_obj_align(state_->title, LV_ALIGN_TOP_LEFT, 134, 24);

        state_->index = lv_label_create(screen);
        lv_obj_set_style_text_color(state_->index, lv_color_hex(0xA0A0B0), 0);
        lv_obj_set_style_text_font(state_->index, &lv_font_montserrat_14, 0);
        lv_obj_align(state_->index, LV_ALIGN_TOP_LEFT, 134, 82);

        // Volume
        auto volume_icon = lv_label_create(screen);
        lv_obj_set_style_text_color(volume_icon, lv_color_white(), 0);
        lv_obj_set_style_text_font(volume_icon, &lv_font_montserrat_14, 0);
        lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
        lv_obj_align(volume_icon, LV_ALIGN_TOP_LEFT, 134, 108);
        auto volume = lv_slider_create(screen);
        lv_obj_set_size(volume, 140, 8);
        lv_obj_align_to(volume, volume_icon, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
        lv_slider_set_range(volume, 0, 100);
        lv_slider_set_value(volume, services_.audio_codec()->output_volume(), LV_ANIM_OFF);
        lv_obj_add_event_cb(volume, OnVolume, LV_EVENT_RELEASED, this);

        state_->progress = lv_bar_create(screen);
        lv_obj_set_size(state_->progress, 288, 6);
        lv_obj_align(state_->progress, LV_ALIGN_TOP_MID, 0, 138);
        lv_bar_set_range(state_->progress, 0, 1000);

        state_->time = lv_label_create(screen);
        lv_obj_set_style_text_color(state_->time, lv_color_hex(0xA0A0B0), 0);
        lv_obj_set_style_text_font(state_->time, &lv_font_montserrat_14, 0);
        lv_label_set_text(state_->time, "00:00");
        lv_obj_align(state_->time, LV_ALIGN_TOP_RIGHT, -16, 148);

        auto play = CreateIconButton(screen, LV_SYMBOL_PAUSE, OnPlayPause, this);
        state_->play_label = lv_obj_get_child(play, 0);
        lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, -14);
        auto prev = CreateIconButton(screen, &desktop_img_prev, OnStep, this);
        lv_obj_set_user_data(prev, reinterpret_cast<void*>(-1));
        lv_obj_align_to(prev, play, LV_ALIGN_OUT_LEFT_MID, -24, 0);
        auto next = CreateIconButton(screen, &desktop_img_next, OnStep, this);
        lv_obj_set_user_data(next, reinterpret_cast<void*>(1));
        lv_obj_align_to(next, play, LV_ALIGN_OUT_RIGHT_MID, 24, 0);

        auto state = state_;
        auto services = &services_;
        StartWorkerTask("music", 10240, 4, [state, services]() { Run(state, *services); });
    }

    void Close() override {
        if (state_) {
            state_->session.Cancel();
        }
    }

private:
    const DesktopServices& services_;
    std::shared_ptr<MusicState> state_;

    static void OnStep(lv_event_t* e) {
        auto self = static_cast<MusicApp*>(lv_event_get_user_data(e));
        auto target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        self->state_->step =
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    }

    static void OnPlayPause(lv_event_t* e) {
        auto self = static_cast<MusicApp*>(lv_event_get_user_data(e));
        bool paused = !self->state_->paused;
        self->state_->paused = paused;
        lv_label_set_text(self->state_->play_label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }

    static void OnVolume(lv_event_t* e) {
        auto self = static_cast<MusicApp*>(lv_event_get_user_data(e));
        auto slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        self->services_.audio_codec()->SetOutputVolume(lv_slider_get_value(slider));
    }

    static void Run(std::shared_ptr<MusicState> state, const DesktopServices& services) {
        RegisterDecoders();
        MediaAudioOutput audio(services);
        int count = static_cast<int>(state->files.size());
        int index = 0;
        while (!state->session.cancelled()) {
            bool finished = PlayFile(state, audio, state->files[index], index, count);
            int step = state->step.exchange(0);
            if (step == 0) {
                step = 1;
            }
            audio.End(finished);
            index = ((index + step) % count + count) % count;
        }
    }

    // Returns true when the track played to the end
    static bool PlayFile(std::shared_ptr<MusicState>& state, MediaAudioOutput& audio,
                         const std::string& path, int index, int count) {
        auto keep_going = [&state]() { return !state->session.cancelled() && state->step == 0; };
        if (!state->session.RunLocked([&]() {
                lv_label_set_text(state->title, BaseName(path).c_str());
                lv_label_set_text_fmt(state->index, "%d / %d", index + 1, count);
                lv_bar_set_value(state->progress, 0, LV_ANIM_OFF);
                lv_label_set_text(state->time, "00:00");
            })) {
            return false;
        }

        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr) {
            state->session.Sleep(1000);
            return false;
        }
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        esp_audio_simple_dec_cfg_t cfg = {};
        cfg.dec_type = DecoderTypeFor(path);
        esp_audio_simple_dec_handle_t decoder = nullptr;
        if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "No decoder for %s", path.c_str());
            fclose(f);
            state->session.Sleep(1000);
            return false;
        }

        auto input = static_cast<uint8_t*>(heap_caps_malloc(kInputChunk, MALLOC_CAP_SPIRAM));
        size_t output_size = kInitialOutputSize;
        auto output = static_cast<uint8_t*>(heap_caps_malloc(output_size, MALLOC_CAP_SPIRAM));
        int sample_rate = 0;
        int channels = 0;
        int bits = 16;
        uint64_t played_frames = 0;
        int64_t last_progress = 0;
        bool finished = false;
        bool failed = input == nullptr || output == nullptr;

        while (!failed && keep_going()) {
            if (state->paused) {
                state->session.Sleep(50);
                continue;
            }
            size_t read = fread(input, 1, kInputChunk, f);
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = input;
            raw.len = read;
            raw.eos = read < kInputChunk;
            while (raw.len > 0 && keep_going()) {
                esp_audio_simple_dec_out_t out = {};
                out.buffer = output;
                out.len = output_size;
                esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    // Rare: grows once to the decoder's frame size
                    heap_caps_free(output);
                    output_size = out.needed_size;
                    output =
                        static_cast<uint8_t*>(heap_caps_malloc(output_size, MALLOC_CAP_SPIRAM));
                    if (output == nullptr) {
                        failed = true;
                        break;
                    }
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "Decode error %d in %s", ret, path.c_str());
                    failed = true;
                    break;
                }
                if (out.decoded_size > 0) {
                    if (sample_rate == 0) {
                        esp_audio_simple_dec_info_t info = {};
                        esp_audio_simple_dec_get_info(decoder, &info);
                        sample_rate = static_cast<int>(info.sample_rate);
                        channels = info.channel;
                        bits = info.bits_per_sample;
                        if ((bits != 16 && bits != 24 && bits != 32) ||
                            !audio.Begin(sample_rate, channels)) {
                            failed = true;
                            break;
                        }
                    }
                    size_t samples = To16Bit(output, out.decoded_size, bits);
                    audio.Write(reinterpret_cast<const int16_t*>(output), samples / channels,
                                keep_going);
                    played_frames += samples / channels;
                }
                if (raw.consumed == 0 && out.decoded_size == 0) {
                    break;
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            if (raw.eos) {
                finished = !failed;
                break;
            }

            int64_t now = esp_timer_get_time();
            if (now - last_progress > kProgressIntervalMs * 1000 && sample_rate > 0) {
                last_progress = now;
                unsigned sec = static_cast<unsigned>(played_frames / sample_rate);
                int permille = file_size > 0 ? static_cast<int>(ftell(f) * 1000LL / file_size) : 0;
                state->session.RunLocked([&]() {
                    lv_bar_set_value(state->progress, permille, LV_ANIM_OFF);
                    lv_label_set_text_fmt(state->time, "%02u:%02u", sec / 60, sec % 60);
                });
            }
        }

        esp_audio_simple_dec_close(decoder);
        heap_caps_free(input);
        heap_caps_free(output);
        fclose(f);
        if (failed) {
            state->session.Sleep(1000);
        }
        return finished;
    }
};

}  // namespace

std::unique_ptr<DesktopApp> CreateMusicApp(const DesktopServices& services) {
    return std::make_unique<MusicApp>(services);
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
