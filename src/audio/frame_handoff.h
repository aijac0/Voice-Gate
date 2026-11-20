#pragma once

#include "audio/frame_assembler.h"

#include <functional>

namespace voice_gate::audio {

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void analyze_reference(const AudioFrame& frame) = 0;
    virtual void set_stream_delay(std::chrono::milliseconds delay) = 0;
    virtual void process_capture(const AudioFrame& frame) = 0;
};

struct FrameTiming {
    AudioTime reference_submission;
    AudioTime scheduled_output;
    AudioTime capture_time;
    AudioTime input_submission;

    [[nodiscard]] AudioTime buffering_delay() const noexcept;
};

class FrameHandoff {
public:
    FrameHandoff(AudioProcessor& processor, std::function<AudioTime()> clock,
                 bool apply_delay);

    [[nodiscard]] FrameTiming submit(const AudioFrame& reference, const AudioFrame& input);

private:
    AudioProcessor& processor_;
    std::function<AudioTime()> clock_;
    bool apply_delay_;
};

}  // namespace voice_gate::audio
