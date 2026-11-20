#include "audio/macos/core_audio_backend.h"
#include "audio/pcm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <thread>
#include <vector>

namespace {

using namespace voice_gate::audio;
using namespace std::chrono_literals;

struct Timing {
    void observe(std::optional<AudioTime> first_sample, AudioTime callback_time, std::size_t frames)
    {
        total_frames += frames;
        ++blocks;
        if (first_sample) {
            if (last_time && *first_sample <= *last_time) {
                ++out_of_order;
            }
            if (!first_time) {
                first_time = *first_sample;
                first_callback = callback_time;
            }
            last_time = *first_sample;
            ++timed_blocks;
        }
    }

    void print(const char* direction, const StreamFormat& format) const
    {
        std::cout << direction << " rate_hz=" << format.sample_rate
                  << " channels=" << format.channels << " blocks=" << blocks
                  << " frames=" << total_frames << " timed_blocks=" << timed_blocks
                  << " out_of_order=" << out_of_order;
        if (first_time) {
            std::cout << " first_sample_ns=" << first_time->count()
                      << " first_callback_ns=" << first_callback.count()
                      << " last_first_sample_ns=" << last_time->count();
        }
        std::cout << '\n';
    }

    [[nodiscard]] bool valid() const
    {
        return blocks > 1 && timed_blocks == blocks && out_of_order == 0;
    }

    std::size_t blocks{};
    std::size_t total_frames{};
    std::size_t timed_blocks{};
    std::size_t out_of_order{};
    std::optional<AudioTime> first_time;
    std::optional<AudioTime> last_time;
    AudioTime first_callback{};
};

std::vector<std::byte> make_tone(const StreamFormat& format)
{
    const auto frames = static_cast<std::size_t>(format.sample_rate);
    const auto stride = format.bytes_per_sample();
    std::vector<std::byte> tone(frames * stride);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double t = frame / format.sample_rate;
        double sample = 0;
        if (t >= 0.25 && t < 0.75) {
            const double envelope = std::min({1.0, (t - 0.25) / 0.01, (0.75 - t) / 0.01});
            sample = 0.035 * envelope * std::sin(2.0 * std::numbers::pi * 440.0 * (t - 0.25));
        }
        write_sample(tone.data() + frame * stride, format.encoding, static_cast<float>(sample));
    }
    return tone;
}

struct Probe {
    Probe(CoreAudioBackend& backend, DeviceFormats formats)
        : backend(backend), formats(formats), tone(make_tone(formats.output)),
          captured(static_cast<std::size_t>(2 * formats.input.sample_rate) *
                   formats.input.bytes_per_sample())
    {
    }

    static void capture(void* context, const InputBlock& block) noexcept
    {
        auto& probe = *static_cast<Probe*>(context);
        probe.input.observe(block.first_sample, probe.backend.now(), block.frames);
        const auto stride = probe.formats.input.bytes_per_sample();
        const auto& buffer = block.buffers.front();
        const auto available_frames = (probe.captured.size() - probe.captured_bytes) / stride;
        for (std::size_t frame = 0; frame < std::min(block.frames, available_frames); ++frame) {
            std::memcpy(probe.captured.data() + probe.captured_bytes,
                        buffer.bytes.data() + frame * buffer.channels * stride, stride);
            probe.captured_bytes += stride;
        }
    }

    static void render(void* context, const OutputBlock& block) noexcept
    {
        auto& probe = *static_cast<Probe*>(context);
        const auto first_frame = probe.output.total_frames;
        probe.output.observe(block.first_sample, probe.backend.now(), block.frames);
        const auto stride = probe.formats.output.bytes_per_sample();
        const auto tone_frames = probe.tone.size() / stride;
        for (const auto& buffer : block.buffers) {
            for (std::size_t frame = 0; frame < block.frames; ++frame) {
                const auto tone_frame = first_frame + frame;
                auto* destination = buffer.bytes.data() + frame * buffer.channels * stride;
                if (tone_frame >= tone_frames) {
                    std::memset(destination, 0, buffer.channels * stride);
                    continue;
                }
                for (unsigned channel = 0; channel < buffer.channels; ++channel) {
                    std::memcpy(destination + channel * stride,
                                probe.tone.data() + tone_frame * stride, stride);
                }
            }
        }
    }

    [[nodiscard]] float input_peak() const noexcept
    {
        float peak = 0;
        const auto stride = formats.input.bytes_per_sample();
        for (std::size_t offset = 0; offset < captured_bytes; offset += stride) {
            peak = std::max(peak, std::abs(read_sample(captured.data() + offset,
                                                      formats.input.encoding)));
        }
        return peak;
    }

    CoreAudioBackend& backend;
    DeviceFormats formats;
    std::vector<std::byte> tone;
    std::vector<std::byte> captured;
    std::size_t captured_bytes{};
    Timing input;
    Timing output;
};

}  // namespace

int main()
{
    try {
        CoreAudioBackend backend;
        const auto formats = backend.open();
        Probe probe(backend, formats);
        backend.start({&probe, &Probe::capture, &Probe::render});
        std::this_thread::sleep_for(1400ms);
        backend.stop();
        probe.input.print("input", formats.input);
        probe.output.print("output", formats.output);
        const float peak = probe.input_peak();
        std::cout << "input_peak=" << peak << '\n';
        if (!probe.input.valid() || !probe.output.valid() || peak == 0 ||
            probe.output.total_frames < probe.tone.size() / formats.output.bytes_per_sample()) {
            std::cerr << "Probe did not verify capture, playback, and ordered device timestamps.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
