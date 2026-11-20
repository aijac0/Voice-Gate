#pragma once

#include "audio/frame_assembler.h"

#include <functional>

namespace voice_gate::audio {

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void analyze_reference(const AudioFrame& frame) = 0;
    virtual void set_stream_delay(std::chrono::milliseconds delay) = 0;
    // Replace the samples in place, preserving the format and timing metadata.
    virtual void process_capture(AudioFrame& frame) = 0;
};

struct FrameTiming {
    AudioTime reference_submission;
    AudioTime scheduled_output;
    AudioTime capture_time;
    AudioTime input_submission;

    [[nodiscard]] AudioTime buffering_delay() const noexcept;
};

struct ReferenceTiming {
    AudioTime submission;
    AudioTime scheduled_output;
};

class FrameHandoff {
public:
    FrameHandoff(AudioProcessor& processor, std::function<AudioTime()> clock,
                 bool apply_delay);

    [[nodiscard]] ReferenceTiming submit_reference(const AudioFrame& reference);
    [[nodiscard]] FrameTiming submit_input(AudioFrame& input, ReferenceTiming reference);
    [[nodiscard]] FrameTiming submit(const AudioFrame& reference, AudioFrame& input);

private:
    AudioProcessor& processor_;
    std::function<AudioTime()> clock_;
    bool apply_delay_;
};

}  // namespace voice_gate::audio
