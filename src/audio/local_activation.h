#pragma once

#include "audio/frame_assembler.h"

namespace voice_gate::audio {

struct SoundTriggerConfig {
    double rms_threshold;
    std::chrono::milliseconds confirmation;
};

struct SoundOnset {
    AudioTime first_loud_sample;
    AudioTime confirmed_through;
};

struct SoundObservation {
    double rms;
    bool sound_present;
    std::optional<SoundOnset> onset;
};

// Consumes processed capture frames in timestamp order on the processing thread.
class SoundTrigger {
public:
    SoundTrigger(FrameFormat format, SoundTriggerConfig config);

    [[nodiscard]] SoundObservation observe(const AudioFrame& frame);
    void reset() noexcept;

private:
    FrameFormat format_;
    std::size_t frames_to_confirm_;
    double rms_threshold_;
    std::optional<AudioTime> previous_frame_;
    AudioTime first_loud_sample_{};
    std::size_t loud_frames_{};
};

class RollingPreBuffer {
public:
    explicit RollingPreBuffer(std::chrono::milliseconds duration = std::chrono::seconds(1));

    void append(AudioFrame frame);
    [[nodiscard]] std::deque<AudioFrame> take();
    void clear() noexcept;

private:
    std::size_t capacity_;
    std::deque<AudioFrame> frames_;
};

}  // namespace voice_gate::audio
