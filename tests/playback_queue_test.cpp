#include "audio/audio_pipeline.h"
#include "audio/pcm.h"
#include "audio/playback_queue.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
using namespace voice_gate::audio;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_native_playback()
{
    constexpr StreamFormat stereo{48000, 2, SampleEncoding::signed16};
    PlaybackQueue queue(stereo, 5);
    const std::array initial{0.25F, -0.5F, 0.75F, 0.125F, -0.25F};
    const std::array more{0.5F, -1.0F};
    require(queue.drained(0ms) && queue.enqueue(initial) && !queue.enqueue(more),
            "Enqueue a complete block or leave the queue as it was");

    std::array<std::int16_t, 6> interleaved{};
    const std::array output{BufferView<std::byte>{std::as_writable_bytes(std::span(interleaved)), 2}};
    queue.render({output, 3, 100ms});
    for (std::size_t frame = 0; frame < 3; ++frame) {
        const auto actual = read_sample(output.front().bytes.data() + frame * 4, stereo.encoding);
        require(interleaved[frame * 2] == interleaved[frame * 2 + 1] &&
                    std::abs(actual - initial[frame]) < 0.00004F,
                "Interleaved playback duplicates the native mono sample across channels");
    }
    require(!queue.drained(200ms) && queue.enqueue(more), "Retain unread samples and reuse consumed slots");

    std::array<std::int16_t, 5> left{};
    std::array<std::int16_t, 5> right{};
    const std::array planar{
        BufferView<std::byte>{std::as_writable_bytes(std::span(left)), 1},
        BufferView<std::byte>{std::as_writable_bytes(std::span(right)), 1}};
    const std::array expected{0.125F, -0.25F, 0.5F, -1.0F, 0.0F};
    queue.render({planar, expected.size(), 101ms});
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
        const auto actual = read_sample(planar.front().bytes.data() + frame * 2, stereo.encoding);
        require(left[frame] == right[frame] && std::abs(actual - expected[frame]) < 0.00004F,
                "Planar playback preserves order and fills the unoccupied suffix with silence");
    }
    require(!queue.drained(101ms + 83333ns) && queue.drained(101ms + 83334ns),
            "Keep playback pending through the scheduled time of the last emitted sample");

    require(queue.enqueue(more), "Queue playback after the previous audio drains");
    queue.render({planar, expected.size(), std::nullopt});
    require(!queue.drained(1s), "A missing device timestamp must not certify playback completion");
}

class Recorder final : public AudioProcessor {
public:
    void analyze_reference(const AudioFrame& frame) override { reference = frame.samples; }
    void set_stream_delay(std::chrono::milliseconds) override {}
    void process_capture(AudioFrame&) override {}

    std::vector<std::vector<float>> reference;
};

void render(void* context, const OutputBlock& block) noexcept
{
    static_cast<PlaybackQueue*>(context)->render(block);
}

void check_reference_matches_device_output()
{
    constexpr DeviceFormats formats{{48000, 1, SampleEncoding::float32},
                                    {48000, 2, SampleEncoding::float32}, 480, 480};
    PlaybackQueue queue(formats.output, 480);
    const std::vector<float> half_frame(240, 0.25F);
    require(queue.enqueue(half_frame), "Queue a partial frame of speaker samples");
    Recorder processor;
    AudioPipeline pipeline(formats, {48000, 1}, {48000, 2}, {&queue, nullptr, render},
                           processor, [] { return 115ms; }, 1);
    const auto callbacks = pipeline.callbacks();
    std::array<float, 480> input{};
    std::array<float, 960> output{};
    const std::array capture{BufferView<const std::byte>{std::as_bytes(std::span(input)), 1}};
    const std::array playback{BufferView<std::byte>{std::as_writable_bytes(std::span(output)), 2}};
    callbacks.capture(callbacks.context, InputBlock{capture, 480, 100ms});
    callbacks.render(callbacks.context, OutputBlock{playback, 480, 110ms});
    const auto batch = pipeline.process(115ms);
    require(!batch.restart_required && batch.captures.size() == 1 && processor.reference.size() == 2,
            "The playback callback supplies an ordered stereo reference to the audio pipeline");
    for (std::size_t frame = 0; frame < 480; ++frame) {
        const float expected = frame < half_frame.size() ? 0.25F : 0.0F;
        require(output[frame * 2] == expected && output[frame * 2 + 1] == expected &&
                    processor.reference[0][frame] == expected && processor.reference[1][frame] == expected,
                "The output reference must contain exactly the samples and silence sent to the device");
    }
}

}  // namespace

int main()
{
    try {
        check_native_playback();
        check_reference_matches_device_output();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
