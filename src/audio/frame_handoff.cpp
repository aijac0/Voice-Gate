#include "audio/frame_handoff.h"

#include <stdexcept>
#include <utility>

namespace voice_gate::audio {

AudioTime FrameTiming::buffering_delay() const noexcept
{
    const auto output_wait = scheduled_output - reference_submission;
    const auto input_wait = input_submission - capture_time;
    return output_wait + input_wait;
}

FrameHandoff::FrameHandoff(AudioProcessor& processor, std::function<AudioTime()> clock,
                           bool apply_delay)
    : processor_(processor), clock_(std::move(clock)), apply_delay_(apply_delay)
{
}

ReferenceTiming FrameHandoff::submit_reference(const AudioFrame& reference)
{
    const auto submission = clock_();
    processor_.analyze_reference(reference);
    return {submission, reference.first_sample};
}

FrameTiming FrameHandoff::submit_input(const AudioFrame& input, ReferenceTiming reference)
{
    const auto input_submission = clock_();
    const FrameTiming timing{reference.submission, reference.scheduled_output,
                             input.first_sample, input_submission};
    if (apply_delay_) {
        const auto delay = std::chrono::round<std::chrono::milliseconds>(timing.buffering_delay());
        if (delay < std::chrono::milliseconds::zero() || delay > std::chrono::milliseconds(500)) {
            throw std::runtime_error("Buffering delay is outside the WebRTC range");
        }
        processor_.set_stream_delay(delay);
    }
    processor_.process_capture(input);
    return timing;
}

FrameTiming FrameHandoff::submit(const AudioFrame& reference, const AudioFrame& input)
{
    return submit_input(input, submit_reference(reference));
}

}  // namespace voice_gate::audio
