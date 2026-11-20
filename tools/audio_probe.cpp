#include "audio/macos/core_audio_backend.h"
#include "audio/pcm.h"

#ifdef VOICE_GATE_ENABLE_WEBRTC
#include "audio/audio_pipeline.h"
#include "audio/webrtc_processor.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numbers>
#include <string_view>
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

#ifdef VOICE_GATE_ENABLE_WEBRTC
class DeviceStop {
public:
    explicit DeviceStop(CoreAudioBackend& backend) : backend_(backend) {}
    ~DeviceStop()
    {
        try {
            backend_.stop();
        } catch (...) {
            // The explicit stop reports failures. During unwinding, stop
            // still detaches callbacks from the probe before it is destroyed.
        }
    }

private:
    CoreAudioBackend& backend_;
};

struct Epoch {
    std::size_t references{};
    std::size_t inputs{};
    std::size_t input_silence{};
    std::size_t reference_silence{};
    std::optional<FrameTiming> first;
    std::optional<FrameTiming> last;
    std::string_view restart;
    float input_peak{};
};

void print_timing(const char* point, std::size_t sequence, const FrameTiming& timing)
{
    std::cout << point << " frame_sequence=" << sequence
              << " reference_submission_ns=" << timing.reference_submission.count()
              << " scheduled_output_ns=" << timing.scheduled_output.count()
              << " capture_time_ns=" << timing.capture_time.count()
              << " input_submission_ns=" << timing.input_submission.count()
              << " buffering_delay_ns=" << timing.buffering_delay().count() << '\n';
}

Epoch run_epoch(CoreAudioBackend& backend, DeviceFormats devices, FrameFormat input,
                FrameFormat reference, std::uint64_t generation,
                std::chrono::steady_clock::time_point deadline)
{
    Probe fixture(backend, devices);
    const auto processor = make_webrtc_processor(input, reference);
    AudioPipeline pipeline(devices, input, reference, {&fixture, Probe::capture, Probe::render},
                           *processor, [&] { return backend.now(); }, generation);
    DeviceStop stop_on_exit(backend);
    backend.start(pipeline.callbacks());

    Epoch epoch;
    auto next_check = std::chrono::steady_clock::now() + 100ms;
    auto last_input = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_check) {
            if (backend.configuration_changed()) {
                epoch.restart = "device-format-change";
                break;
            }
            next_check = now + 100ms;
        }
        const auto batch = pipeline.process(backend.now());
        epoch.references += batch.reference_frames;
        epoch.inputs += batch.input_timings.size();
        epoch.input_silence += batch.input_silence_frames;
        epoch.reference_silence += batch.reference_silence_frames;
        if (!batch.input_timings.empty()) {
            last_input = now;
            if (!epoch.first) {
                epoch.first = batch.input_timings.front();
            }
            epoch.last = batch.input_timings.back();
        }
        if (batch.restart_required || now - last_input >= 500ms) {
            epoch.restart = batch.restart_required ? "audio-discontinuity" : "audio-stalled";
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    backend.stop();
    epoch.input_peak = fixture.input_peak();

    std::cout << "generation=" << generation
              << " output_device=" << backend.output_device_id()
              << " native_input_channels=" << devices.input.channels
              << " native_output_channels=" << devices.output.channels
              << " apm_rate_hz=" << input.sample_rate
              << " apm_input_channels=" << input.channels
              << " apm_reference_channels=" << reference.channels
              << " references=" << epoch.references << " inputs=" << epoch.inputs
              << " input_silence_frames=" << epoch.input_silence
              << " reference_silence_frames=" << epoch.reference_silence
              << " input_peak=" << epoch.input_peak;
    if (!epoch.restart.empty()) {
        std::cout << " restart=" << epoch.restart;
    }
    std::cout << '\n';
    if (epoch.first) {
        print_timing("first", 1, *epoch.first);
        print_timing("last", epoch.inputs, *epoch.last);
    }
    return epoch;
}

DeviceFormats wait_for_devices(CoreAudioBackend& backend, CoreAudioOutput output,
                               std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        try {
            return backend.open(output);
        } catch (const std::runtime_error&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(25ms);
        }
    }
}

enum class TestTransition { none, restart, switch_output };

int verify_webrtc(TestTransition transition)
{
    CoreAudioBackend backend;
    auto devices = backend.open();
    const auto initial_output = backend.output_device_id();
    const FrameFormat input{48000, devices.input.channels};
    const FrameFormat reference{48000, devices.output.channels};
    const unsigned phases = transition == TestTransition::none ? 1 : 2;
    std::uint64_t generation = 0;
    for (unsigned phase = 0; phase < phases; ++phase) {
        const auto deadline = std::chrono::steady_clock::now() + 1400ms;
        const auto output = phase == 1 && transition == TestTransition::switch_output
            ? CoreAudioOutput::system_device : CoreAudioOutput::default_device;
        std::size_t frames_processed = 0;
        bool recovered = false;
        bool captured_signal = false;
        do {
            if (++generation > 8) {
                throw std::runtime_error("Repeated audio interruption prevented verification");
            }
            if (generation > 1) {
                devices = wait_for_devices(backend, output, deadline);
            }
            if (output == CoreAudioOutput::system_device &&
                backend.output_device_id() == initial_output) {
                throw std::runtime_error("The default and system output are the same device");
            }
            const auto epoch = run_epoch(backend, devices, input, reference, generation, deadline);
            frames_processed += epoch.inputs;
            captured_signal = captured_signal || epoch.input_peak > 0;
            recovered = epoch.restart.empty();
        } while (!recovered && std::chrono::steady_clock::now() < deadline);
        if (!recovered || frames_processed < 10 || !captured_signal) {
            throw std::runtime_error("WebRTC did not receive a sustained run of complete frames");
        }
        if (phase + 1 < phases) {
            std::cout << (transition == TestTransition::switch_output
                ? "Switching this probe to the system output device.\n"
                : "Restarting Core Audio for a second device run.\n");
        }
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, [[maybe_unused]] char** argv)
{
    try {
        if (argc > 1) {
#ifdef VOICE_GATE_ENABLE_WEBRTC
            if (std::string_view(argv[1]) == "--verify-webrtc") {
                if (argc == 2) {
                    return verify_webrtc(TestTransition::none);
                }
                if (argc == 3 && std::string_view(argv[2]) == "--restart") {
                    return verify_webrtc(TestTransition::restart);
                }
                if (argc == 3 && std::string_view(argv[2]) == "--switch-output") {
                    return verify_webrtc(TestTransition::switch_output);
                }
            }
            std::cerr << "Usage: voice_gate_audio_probe [--verify-webrtc [--restart | --switch-output]]\n";
#else
            std::cerr << "Build with VOICE_GATE_ENABLE_WEBRTC for WebRTC timing verification.\n";
#endif
            return 1;
        }
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
