#include "audio/webrtc_processor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

namespace {

constexpr unsigned sample_rate = 48000;
constexpr std::size_t samples_per_frame = sample_rate / 100;
constexpr FrameFormat input_format{sample_rate, 1};
constexpr FrameFormat reference_format{sample_rate, 2};

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_bypass()
{
    const auto processor = make_webrtc_processor(input_format, reference_format);
    const AudioFrame reference{106ms, {std::vector<float>(samples_per_frame, 0.05F),
                                       std::vector<float>(samples_per_frame, -0.05F)}, 0};
    const std::vector<float> original(samples_per_frame, 0.02F);
    AudioFrame input{130ms, {original}, 0};
    const std::array<AudioTime, 2> times{100ms, 134ms};
    std::size_t next = 0;
    FrameHandoff handoff(*processor, [&] { return times.at(next++); });
    const auto timing = handoff.submit(reference, input);
    require(timing && timing->buffering_delay() == 10ms && !timing->submitted_delay &&
                input.samples.front() == original && !processor->estimated_echo_delay(),
            "Effects-disabled WebRTC must return the original samples and submit no delay");
}

std::vector<float> fixture(std::size_t count, std::uint32_t seed)
{
    std::vector<float> samples(count);
    float smoothed = 0.0F;
    for (auto& sample : samples) {
        seed = seed * 1664525u + 1013904223u;
        const float white = static_cast<float>(seed >> 16) / 32768.0F - 1.0F;
        smoothed = 0.65F * smoothed + 0.35F * white;
        sample = 0.75F * smoothed;
    }
    return samples;
}

std::vector<float> voiced_fixture(std::size_t count)
{
    std::vector<float> samples(count);
    double phase = 0.0;
    constexpr double turn = 2.0 * std::numbers::pi;
    for (std::size_t index = 0; index < count; ++index) {
        const double seconds = static_cast<double>(index) / sample_rate;
        phase += turn * (170.0 + 30.0 * std::sin(turn * 2.1 * seconds)) / sample_rate;
        const double envelope = 0.6 + 0.4 * std::sin(turn * 4.1 * seconds);
        samples[index] = static_cast<float>(envelope *
            (0.35 * std::sin(phase) + 0.17 * std::sin(2 * phase) + 0.08 * std::sin(4 * phase)));
    }
    return samples;
}

void check_echo_path(std::chrono::milliseconds signal_delay)
{
    constexpr std::size_t frame_count = 650;
    constexpr std::size_t double_talk_start = 450 * samples_per_frame;
    constexpr std::size_t double_talk_end = 570 * samples_per_frame;
    const auto sample_count = frame_count * samples_per_frame;
    const auto left = fixture(sample_count, 0x708c4be3);
    const auto right = fixture(sample_count, 0x8f13f0e1);
    const auto near_end = voiced_fixture(sample_count);
    const auto delay_samples = static_cast<std::size_t>(signal_delay.count()) * sample_rate / 1000;
    const auto processor = make_webrtc_processor(input_format, reference_format,
                                                 EchoCancellation::enabled);
    AudioTime clock{};
    FrameHandoff handoff(*processor, [&] { return clock; });
    std::vector<float> cleaned;
    cleaned.reserve(sample_count);
    double raw_power = 0.0;
    double cleaned_power = 0.0;
    std::optional<std::chrono::milliseconds> estimated_delay;

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        const auto begin = frame * samples_per_frame;
        const auto end = begin + samples_per_frame;
        const auto start = 1000ms + 10ms * static_cast<std::int64_t>(frame);
        const AudioFrame reference{start + 6ms, {{left.begin() + begin, left.begin() + end},
                                                 {right.begin() + begin, right.begin() + end}}, 0};
        AudioFrame input{start + 6ms, {std::vector<float>(samples_per_frame)}, 0};
        auto& samples = input.samples.front();
        for (std::size_t index = begin; index < end; ++index) {
            float sample = 0.0F;
            if (index >= delay_samples) {
                sample += 0.46F * left[index - delay_samples] +
                          0.14F * right[index - delay_samples];
            }
            constexpr std::size_t reflection_samples = 144;
            if (index >= delay_samples + reflection_samples) {
                sample += 0.12F * left[index - delay_samples - reflection_samples];
            }
            if (index >= double_talk_start && index < double_talk_end) {
                sample += near_end[index];
            }
            samples[index - begin] = sample;
        }

        const bool settled_echo_only = frame >= 180 && frame < 420;
        if (settled_echo_only) {
            raw_power += std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
        }
        clock = start;
        const auto render_timing = handoff.submit_reference(reference);
        clock = start + 10ms;
        const auto timing = handoff.submit_input(input, render_timing);
        require(timing && timing->submitted_delay == 10ms,
                "Keep the buffering estimate fixed while changing the simulated echo path");
        if (settled_echo_only) {
            cleaned_power += std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
        }
        // The pinned APM stats queue has one entry. Poll it throughout the run.
        if ((frame + 1) % 20 == 0 && frame < 420) {
            estimated_delay = processor->estimated_echo_delay();
        }
        cleaned.insert(cleaned.end(), samples.begin(), samples.end());
    }

    // APM's internal filters can shift the emitted waveform slightly. Find the
    // near-end component over the central 600 ms with up to one frame of lag.
    const auto near_begin = double_talk_start + 40 * samples_per_frame;
    const auto near_end_index = double_talk_end - 20 * samples_per_frame;
    const auto near_power = std::inner_product(near_end.begin() + near_begin,
                                               near_end.begin() + near_end_index,
                                               near_end.begin() + near_begin, 0.0);
    double near_gain = 0.0;
    for (std::size_t lag = 0; lag <= samples_per_frame; ++lag) {
        const auto matched = std::inner_product(near_end.begin() + near_begin,
                                                 near_end.begin() + near_end_index,
                                                 cleaned.begin() + near_begin + lag, 0.0);
        near_gain = std::max(near_gain, matched / near_power);
    }
    const auto attenuation = 10.0 * std::log10(raw_power / cleaned_power);
    std::cout << "signal_delay_ms=" << signal_delay.count()
              << " apm_delay_ms=" << estimated_delay.value_or(-1ms).count()
              << " echo_attenuation_db=" << attenuation << " near_end_gain=" << near_gain << '\n';
    require(estimated_delay && std::chrono::abs(*estimated_delay - signal_delay) <= 12ms,
            "AEC3 must align against the delayed reference independently of the buffering estimate");
    require(std::isfinite(attenuation) && attenuation >= 12.0,
            "AEC3 must substantially attenuate the settled stereo echo");
    require(near_gain >= 0.5, "AEC3 must preserve the independent near-end signal during double-talk");
}

}  // namespace

int main()
{
    try {
        check_bypass();
        check_echo_path(20ms);
        check_echo_path(84ms);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
