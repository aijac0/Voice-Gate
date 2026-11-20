#include "audio/frame_handoff.h"

#include <utility>

namespace voice_gate::audio {

AudioTime FrameTiming::buffering_delay() const noexcept
{
    const auto output_wait = scheduled_output - reference_submission;
    const auto input_wait = input_submission - capture_time;
    return output_wait + input_wait;
}

FrameHandoff::FrameHandoff(AudioProcessor& processor, std::function<AudioTime()> clock)
    : processor_(processor), clock_(std::move(clock)),
      apply_delay_(processor.echo_cancellation_enabled())
{
}

ReferenceTiming FrameHandoff::submit_reference(const AudioFrame& reference)
{
    const auto submission = clock_();
    processor_.analyze_reference(reference);
    return {submission, reference.first_sample};
}

std::optional<FrameTiming> FrameHandoff::submit_input(AudioFrame& input, ReferenceTiming reference)
{
    const auto input_submission = clock_();
    FrameTiming timing{reference.submission, reference.scheduled_output,
                       input.first_sample, input_submission, std::nullopt};
    if (apply_delay_) {
        const auto delay = timing.buffering_delay();
        if (input_submission < input.first_sample || input_submission < reference.submission ||
            delay < AudioTime::zero() || delay > std::chrono::milliseconds(500)) {
            return std::nullopt;
        }
        timing.submitted_delay = std::chrono::round<std::chrono::milliseconds>(delay);
        processor_.set_stream_delay(*timing.submitted_delay);
    }
    processor_.process_capture(input);
    return timing;
}

std::optional<FrameTiming> FrameHandoff::submit(const AudioFrame& reference, AudioFrame& input)
{
    return submit_input(input, submit_reference(reference));
}

}  // namespace voice_gate::audio
