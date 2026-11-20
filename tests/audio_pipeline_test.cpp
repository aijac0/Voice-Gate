#include "audio/audio_pipeline.h"
#include "audio/pcm.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

namespace {

class Processor final : public AudioProcessor {
public:
    explicit Processor(bool echo_enabled = false) : echo_enabled(echo_enabled) {}

    bool echo_cancellation_enabled() const noexcept override { return echo_enabled; }
    void analyze_reference(const AudioFrame& frame) override
    {
        reference = frame.samples;
        calls.push_back('R');
    }
    void set_stream_delay(std::chrono::milliseconds delay) override
    {
        calls.push_back('D');
        submitted_delay = delay;
    }
    void process_capture(AudioFrame& frame) override
    {
        input = frame.samples;
        calls.push_back('I');
        frame.samples.front().front() = 0.125F;
    }

    std::vector<std::vector<float>> reference;
    std::vector<std::vector<float>> input;
    std::vector<char> calls;
    std::optional<std::chrono::milliseconds> submitted_delay;
    bool echo_enabled;
};

void fill(void*, const OutputBlock& block) noexcept
{
    for (const auto& buffer : block.buffers) {
        for (std::size_t byte = 0; byte < buffer.bytes.size(); byte += sizeof(float)) {
            write_sample(buffer.bytes.data() + byte, SampleEncoding::float32, 0.25F);
        }
    }
}

void feed(DeviceCallbacks callbacks, unsigned input_channels, unsigned output_channels,
          std::size_t frames, AudioTime time)
{
    const std::vector<float> microphone(frames * input_channels, 0.5F);
    std::vector<float> speaker(frames * output_channels, 0.0F);
    const std::array capture_buffers{
        BufferView<const std::byte>{std::as_bytes(std::span(microphone)), input_channels}};
    const std::array render_buffers{
        BufferView<std::byte>{std::as_writable_bytes(std::span(speaker)), output_channels}};
    callbacks.capture(callbacks.context, InputBlock{capture_buffers, frames, time});
    callbacks.render(callbacks.context, OutputBlock{render_buffers, frames, time + 10ms});
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main()
{
    try {
        constexpr FrameFormat input{48000, 1};
        constexpr FrameFormat reference{48000, 2};
        constexpr DeviceCallbacks fixture{nullptr, nullptr, fill};
        Processor initial;
        AudioPipeline pipeline({{48000, 1, SampleEncoding::float32},
                                {48000, 2, SampleEncoding::float32}, 480, 480},
                               input, reference, fixture, initial, [] { return 115ms; }, 1);
        feed(pipeline.callbacks(), 1, 2, 480, 100ms);
        const auto first = pipeline.process(115ms);
        require(!first.restart_required && first.captures.size() == 1 &&
                    initial.calls == std::vector{'R', 'I'} &&
                    initial.reference[0][0] == 0.25F && initial.input[0][0] == 0.5F,
                "Copy playback after render and submit the reference before capture");
        require(first.captures.front().frame.samples[0][0] == 0.125F &&
                    first.captures.front().frame.first_sample == 100ms &&
                    first.captures.front().timing.capture_time == 100ms,
                "Return the processed samples with their original capture time");

        feed(pipeline.callbacks(), 1, 2, 240, 110ms);
        const auto partial = pipeline.process(180ms);
        require(!partial.restart_required && partial.captures.size() == 1 &&
                    partial.input_silence_frames == 240 && partial.reference_silence_frames == 240,
                "Pad isolated shortfalls at the deadline");

        feed(pipeline.callbacks(), 1, 2, 480, 160ms);
        require(pipeline.process(175ms).restart_required, "Recover from a sustained gap");

        Processor changed;
        AudioPipeline recovered({{44100, 2, SampleEncoding::float32},
                                 {44100, 1, SampleEncoding::float32}, 441, 441},
                                input, reference, fixture, changed, [] { return 215ms; }, 2);
        feed(recovered.callbacks(), 2, 1, 441, 200ms);
        const auto after = recovered.process(215ms);
        require(!after.restart_required && after.captures.size() == 1 &&
                    changed.calls == std::vector{'R', 'I'} &&
                    changed.input.size() == 1 && changed.reference.size() == 2 &&
                    changed.input[0].size() == 480 && changed.reference[1][0] == 0.25F,
                "A restarted route must keep the original APM rate and layouts");

        Processor echo(true);
        AudioTime clock = 115ms;
        AudioPipeline echo_pipeline({{48000, 1, SampleEncoding::float32},
                                     {48000, 2, SampleEncoding::float32}, 480, 480},
                                    input, reference, fixture, echo, [&] { return clock; }, 3);
        feed(echo_pipeline.callbacks(), 1, 2, 480, 100ms);
        const auto enabled = echo_pipeline.process(115ms);
        require(!enabled.restart_required && enabled.captures.size() == 1 &&
                    echo.calls == std::vector{'R', 'D', 'I'} && echo.submitted_delay == 10ms &&
                    enabled.captures.front().timing.submitted_delay == 10ms,
                "Echo mode updates APM delay through the production pipeline");

        clock = 105ms;
        feed(echo_pipeline.callbacks(), 1, 2, 480, 110ms);
        const auto invalid = echo_pipeline.process(125ms);
        require(invalid.restart_required && invalid.captures.empty(),
                "A capture time ahead of the processing clock invalidates the run");
        clock = 135ms;
        feed(echo_pipeline.callbacks(), 1, 2, 480, 120ms);
        require(echo_pipeline.process(135ms).restart_required &&
                    echo.calls == std::vector{'R', 'D', 'I', 'R'},
                "Invalid timing prevents reuse of the current processor run");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
