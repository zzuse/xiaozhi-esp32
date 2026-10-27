#include <sdkconfig.h>

#if CONFIG_ATK_DNESP32S3_BOX_DESKTOP
#include "media_audio.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>

#include "application.h"
#include "audio_codec.h"
#include "device_state.h"

#define TAG "MediaAudio"

static constexpr int kBufferMs = 500;
static constexpr int kChunkMs = 20;
static constexpr size_t kMaxFramesPerStep = 1024;
static constexpr int kSpeakerWaitMs = 2000;
static constexpr int kWriterStopWaitMs = 2000;

static SemaphoreHandle_t SpeakerSemaphore() {
    static SemaphoreHandle_t sem = [] {
        SemaphoreHandle_t s = xSemaphoreCreateBinary();
        xSemaphoreGive(s);
        return s;
    }();
    return sem;
}

static void SetWakeWordPaused(bool paused, AudioCodec* codec) {
    auto& app = Application::GetInstance();
    app.Schedule([paused, codec]() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        auto& audio_service = app.GetAudioService();
        audio_service.EnableWakeWordDetection(!paused);
        if (!paused && audio_service.IsPlaybackIdle()) {
            // AudioService powers the output down by itself only after it used it
            codec->EnableOutput(false);
        }
    });
}

MediaAudioOutput::MediaAudioOutput(const DesktopServices& services) : services_(services) {}

MediaAudioOutput::~MediaAudioOutput() { End(false); }

bool MediaAudioOutput::Begin(int sample_rate, int channels) {
    End(false);
    if (sample_rate <= 0 || channels <= 0 || channels > 2) {
        return false;
    }
    if (xSemaphoreTake(SpeakerSemaphore(), pdMS_TO_TICKS(kSpeakerWaitMs)) != pdTRUE) {
        ESP_LOGW(TAG, "Speaker is busy");
        return false;
    }
    owns_speaker_ = true;

    codec_ = services_.audio_codec();
    in_channels_ = channels;
    out_channels_ = codec_->output_channels();
    out_sample_rate_ = codec_->output_sample_rate();

    if (in_channels_ != out_channels_) {
        esp_ae_ch_cvt_cfg_t cfg = {};
        cfg.sample_rate = static_cast<uint32_t>(sample_rate);
        cfg.bits_per_sample = 16;
        cfg.src_ch = static_cast<uint8_t>(in_channels_);
        cfg.dest_ch = static_cast<uint8_t>(out_channels_);
        if (esp_ae_ch_cvt_open(&cfg, &ch_cvt_) != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "Unsupported channel conversion %d -> %d", in_channels_, out_channels_);
            ReleaseResources();
            return false;
        }
        ch_buf_.resize(kMaxFramesPerStep * out_channels_);
    }
    if (sample_rate != out_sample_rate_) {
        esp_ae_rate_cvt_cfg_t cfg = {};
        cfg.src_rate = static_cast<uint32_t>(sample_rate);
        cfg.dest_rate = static_cast<uint32_t>(out_sample_rate_);
        cfg.channel = static_cast<uint8_t>(out_channels_);
        cfg.bits_per_sample = 16;
        cfg.complexity = 2;
        cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        uint32_t max_out = 0;
        if (esp_ae_rate_cvt_open(&cfg, &rate_cvt_) != ESP_AE_ERR_OK ||
            esp_ae_rate_cvt_get_max_out_sample_num(rate_cvt_, kMaxFramesPerStep, &max_out) !=
                ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "Unsupported sample rate %d -> %d", sample_rate, out_sample_rate_);
            ReleaseResources();
            return false;
        }
        rate_buf_.resize(max_out * out_channels_);
    }

    size_t bytes_per_ms = out_sample_rate_ / 1000 * out_channels_ * sizeof(int16_t);
    size_t capacity = bytes_per_ms * kBufferMs;
    stream_storage_ = static_cast<uint8_t*>(heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM));
    if (stream_storage_ == nullptr) {
        ReleaseResources();
        return false;
    }
    stream_ = xStreamBufferCreateStatic(capacity, 1, stream_storage_, &stream_struct_);
    writer_done_ = xSemaphoreCreateBinary();

    services_.set_media_output_hold(true);
    codec_->EnableOutput(true);
    SetWakeWordPaused(true, codec_);

    stop_ = false;
    discard_ = false;
    active_ = true;
    if (!StartWorkerTask("media_audio", 4096, 6, [this]() { WriterTask(); })) {
        active_ = false;
        services_.set_media_output_hold(false);
        SetWakeWordPaused(false, codec_);
        ReleaseResources();
        return false;
    }
    ESP_LOGI(TAG, "Playing %d Hz x%d -> %d Hz x%d", sample_rate, in_channels_, out_sample_rate_,
             out_channels_);
    return true;
}

void MediaAudioOutput::WriterTask() {
    size_t chunk_samples = out_sample_rate_ / 1000 * kChunkMs * out_channels_;
    std::vector<int16_t> chunk(chunk_samples);
    while (true) {
        chunk.resize(chunk_samples);
        size_t bytes = xStreamBufferReceive(stream_, chunk.data(), chunk_samples * sizeof(int16_t),
                                            pdMS_TO_TICKS(50));
        if (bytes > 0 && !discard_) {
            chunk.resize(bytes / sizeof(int16_t));
            codec_->OutputData(chunk);
            continue;
        }
        if (stop_ && (discard_ || xStreamBufferIsEmpty(stream_))) {
            break;
        }
    }
    xSemaphoreGive(writer_done_);
}

bool MediaAudioOutput::Write(const int16_t* pcm, size_t frames,
                             const std::function<bool()>& keep_going) {
    if (!active_) {
        return false;
    }
    while (frames > 0) {
        size_t step = std::min(frames, kMaxFramesPerStep);
        const int16_t* data = pcm;
        uint32_t out_frames = step;
        if (ch_cvt_ != nullptr) {
            esp_ae_ch_cvt_process(ch_cvt_, step, (esp_ae_sample_t)data, ch_buf_.data());
            data = ch_buf_.data();
        }
        if (rate_cvt_ != nullptr) {
            out_frames = rate_buf_.size() / out_channels_;
            esp_ae_rate_cvt_process(rate_cvt_, (esp_ae_sample_t)data, step, rate_buf_.data(),
                                    &out_frames);
            data = rate_buf_.data();
        }

        auto bytes = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = out_frames * out_channels_ * sizeof(int16_t);
        while (remaining > 0) {
            if (!keep_going()) {
                return false;
            }
            size_t sent = xStreamBufferSend(stream_, bytes, remaining, pdMS_TO_TICKS(50));
            bytes += sent;
            remaining -= sent;
        }
        pcm += step * in_channels_;
        frames -= step;
    }
    return true;
}

void MediaAudioOutput::End(bool drain) {
    if (active_) {
        discard_ = !drain;
        stop_ = true;
        if (xSemaphoreTake(writer_done_, pdMS_TO_TICKS(kWriterStopWaitMs)) != pdTRUE) {
            // Never free buffers the writer may still use
            ESP_LOGE(TAG, "Audio writer did not stop");
            return;
        }
        active_ = false;
        services_.set_media_output_hold(false);
        SetWakeWordPaused(false, codec_);
    }
    ReleaseResources();
}

void MediaAudioOutput::ReleaseResources() {
    if (ch_cvt_ != nullptr) {
        esp_ae_ch_cvt_close(ch_cvt_);
        ch_cvt_ = nullptr;
    }
    if (rate_cvt_ != nullptr) {
        esp_ae_rate_cvt_close(rate_cvt_);
        rate_cvt_ = nullptr;
    }
    ch_buf_.clear();
    ch_buf_.shrink_to_fit();
    rate_buf_.clear();
    rate_buf_.shrink_to_fit();
    if (stream_ != nullptr) {
        vStreamBufferDelete(stream_);
        stream_ = nullptr;
    }
    if (stream_storage_ != nullptr) {
        heap_caps_free(stream_storage_);
        stream_storage_ = nullptr;
    }
    if (writer_done_ != nullptr) {
        vSemaphoreDelete(writer_done_);
        writer_done_ = nullptr;
    }
    if (owns_speaker_) {
        owns_speaker_ = false;
        xSemaphoreGive(SpeakerSemaphore());
    }
}

#endif  // CONFIG_ATK_DNESP32S3_BOX_DESKTOP
