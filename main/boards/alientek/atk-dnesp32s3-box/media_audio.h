#ifndef MEDIA_AUDIO_H
#define MEDIA_AUDIO_H

#include <esp_ae_ch_cvt.h>
#include <esp_ae_rate_cvt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "desktop_app.h"

// Plays media PCM through the board codec while Xiaozhi is idle.
//
// Only one player owns the speaker at a time. While it does, wake word detection is
// paused and the codec output is held on. PCM is converted to the codec format and
// queued in a bounded buffer drained by a dedicated task, so Write() paces the caller.
class MediaAudioOutput {
public:
    explicit MediaAudioOutput(const DesktopServices& services);
    ~MediaAudioOutput();

    bool Begin(int sample_rate, int channels);
    // Interleaved 16-bit PCM. Blocks while the buffer is full; returns false once
    // keep_going() turns false.
    bool Write(const int16_t* pcm, size_t frames, const std::function<bool()>& keep_going);
    // Stops output. With drain, buffered audio is played first.
    void End(bool drain);
    bool active() const { return active_; }

private:
    const DesktopServices& services_;
    AudioCodec* codec_ = nullptr;
    bool active_ = false;
    bool owns_speaker_ = false;
    int in_channels_ = 0;
    int out_channels_ = 0;
    int out_sample_rate_ = 0;

    esp_ae_ch_cvt_handle_t ch_cvt_ = nullptr;
    esp_ae_rate_cvt_handle_t rate_cvt_ = nullptr;
    std::vector<int16_t> ch_buf_;
    std::vector<int16_t> rate_buf_;

    uint8_t* stream_storage_ = nullptr;
    StaticStreamBuffer_t stream_struct_;
    StreamBufferHandle_t stream_ = nullptr;
    SemaphoreHandle_t writer_done_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> discard_{false};

    void WriterTask();
    void ReleaseResources();
};

#endif  // MEDIA_AUDIO_H
