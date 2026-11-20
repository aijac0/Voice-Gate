#pragma once

#include "audio/frame_assembler.h"

#include <functional>
#include <optional>

namespace voice_gate::audio {

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    [[nodiscard]] virtual bool echo_cancellation_enabled() const noexcept { return false; }
    [[nodiscard]] virtual std::optional<std::chrono::milliseconds> estimated_echo_delay()
    {
        return std::nullopt;
    }
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
    std::optional<std::chrono::milliseconds> submitted_delay;

    [[nodiscard]] AudioTime buffering_delay() const noexcept;
};

struct ReferenceTiming {
    AudioTime submission;
    AudioTime scheduled_output;
};

class FrameHandoff {
public:
    FrameHandoff(AudioProcessor& processor, std::function<AudioTime()> clock);

    [[nodiscard]] ReferenceTiming submit_reference(const AudioFrame& reference);
    // A missing result means the timing cannot be used for echo cancellation.
    [[nodiscard]] std::optional<FrameTiming> submit_input(AudioFrame& input,
                                                          ReferenceTiming reference);
    [[nodiscard]] std::optional<FrameTiming> submit(const AudioFrame& reference, AudioFrame& input);

private:
    AudioProcessor& processor_;
    std::function<AudioTime()> clock_;
    const bool apply_delay_;
};

}  // namespace voice_gate::audio
