#include "audio/macos/core_audio_backend.h"
#include "audio/pcm.h"
#include "audio/playback_queue.h"

#ifdef VOICE_GATE_ENABLE_WEBRTC
#include "audio/audio_pipeline.h"
#include "audio/webrtc_processor.h"

#include <CoreFoundation/CoreFoundation.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>
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

std::vector<float> make_tone(const StreamFormat& format)
{
    const auto frames = static_cast<std::size_t>(format.sample_rate);
    std::vector<float> tone(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double t = frame / format.sample_rate;
        double sample = 0;
        if (t >= 0.25 && t < 0.75) {
            const double envelope = std::min({1.0, (t - 0.25) / 0.01, (0.75 - t) / 0.01});
            sample = 0.035 * envelope * std::sin(2.0 * std::numbers::pi * 440.0 * (t - 0.25));
        }
        tone[frame] = static_cast<float>(sample);
    }
    return tone;
}

struct Probe {
    Probe(CoreAudioBackend& backend, DeviceFormats formats)
        : Probe(backend, formats, make_tone(formats.output))
    {
    }

    Probe(CoreAudioBackend& backend, DeviceFormats formats, const std::vector<float>& fixture)
        : backend(backend), formats(formats), playback(formats.output, fixture.size()),
          playback_frames(fixture.size()),
          captured(static_cast<std::size_t>(2 * formats.input.sample_rate) *
                   formats.input.bytes_per_sample())
    {
        if (!playback.enqueue(fixture)) {
            throw std::runtime_error("Could not queue the playback fixture");
        }
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
        probe.output.observe(block.first_sample, probe.backend.now(), block.frames);
        probe.playback.render(block);
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
    PlaybackQueue playback;
    std::size_t playback_frames;
    std::vector<std::byte> captured;
    std::size_t captured_bytes{};
    Timing input;
    Timing output;
};

#ifdef VOICE_GATE_ENABLE_WEBRTC
std::vector<float> make_echo_fixture(const StreamFormat& format)
{
    const auto frames = static_cast<std::size_t>(5 * format.sample_rate);
    std::vector<float> playback(frames);
    std::uint32_t seed = 0xbd14a869;
    float smoothed = 0.0F;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double seconds = frame / format.sample_rate;
        float sample = 0.0F;
        if (seconds >= 0.5 && seconds < 4.5) {
            seed = seed * 1664525u + 1013904223u;
            const float white = static_cast<float>(seed >> 16) / 32768.0F - 1.0F;
            smoothed = 0.8F * smoothed + 0.2F * white;
            const auto ramp = std::min({1.0, (seconds - 0.5) / 0.05, (4.5 - seconds) / 0.05});
            sample = static_cast<float>(0.16 * ramp) * smoothed;
        }
        playback[frame] = sample;
    }
    return playback;
}

struct Energy {
    void add(const AudioFrame& frame)
    {
        for (const auto& channel : frame.samples) {
            sum = std::inner_product(channel.begin(), channel.end(), channel.begin(), sum);
            samples += channel.size();
        }
    }

    [[nodiscard]] double mean() const { return samples > 0 ? sum / samples : 0.0; }

    double sum{};
    std::size_t samples{};
};

struct EchoLevels {
    [[nodiscard]] double reduction_db() const { return 10.0 * std::log10(raw.mean() / clean.mean()); }

    Energy quiet;
    Energy raw;
    Energy clean;
    std::size_t measured_frames{};
};

class EchoProbe final : public AudioProcessor {
public:
    explicit EchoProbe(AudioProcessor& processor) : processor_(processor) {}

    bool echo_cancellation_enabled() const noexcept override { return true; }
    void analyze_reference(const AudioFrame& frame) override { processor_.analyze_reference(frame); }
    void set_stream_delay(std::chrono::milliseconds delay) override { processor_.set_stream_delay(delay); }

    void process_capture(AudioFrame& frame) override
    {
        first_capture_ = first_capture_.value_or(frame.first_sample);
        const auto elapsed = frame.first_sample - *first_capture_;
        const bool usable = frame.silence_frames == 0;
        const bool settled = usable && elapsed >= 2s && elapsed < 4s;
        if (usable && elapsed >= 100ms && elapsed < 350ms) {
            levels.quiet.add(frame);
        }
        if (settled) {
            levels.raw.add(frame);
            ++levels.measured_frames;
        }
        processor_.process_capture(frame);
        if (settled) {
            levels.clean.add(frame);
        }
    }

    EchoLevels levels;

private:
    AudioProcessor& processor_;
    std::optional<AudioTime> first_capture_;
};

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
    EchoLevels echo;
    std::optional<std::chrono::milliseconds> apm_delay;
};

void print_timing(const char* point, std::size_t sequence, const FrameTiming& timing)
{
    std::cout << point << " frame_sequence=" << sequence
              << " reference_submission_ns=" << timing.reference_submission.count()
              << " scheduled_output_ns=" << timing.scheduled_output.count()
              << " capture_time_ns=" << timing.capture_time.count()
              << " input_submission_ns=" << timing.input_submission.count()
              << " buffering_delay_ns=" << timing.buffering_delay().count();
    if (timing.submitted_delay) {
        std::cout << " submitted_delay_ms=" << timing.submitted_delay->count();
    }
    std::cout << '\n';
}

Epoch run_epoch(CoreAudioBackend& backend, DeviceFormats devices, FrameFormat input,
                FrameFormat reference, std::uint64_t generation,
                std::chrono::steady_clock::time_point deadline, EchoCancellation echo)
{
    const bool echo_enabled = echo == EchoCancellation::enabled;
    Probe fixture(backend, devices, echo_enabled ? make_echo_fixture(devices.output)
                                                  : make_tone(devices.output));
    const auto processor = make_webrtc_processor(input, reference, echo);
    EchoProbe echo_probe(*processor);
    AudioProcessor& capture_processor = echo_enabled ? echo_probe : *processor;
    AudioPipeline pipeline(devices, input, reference, {&fixture, Probe::capture, Probe::render},
                           capture_processor, [&] { return backend.now(); }, generation);
    DeviceStop stop_on_exit(backend);
    backend.start(pipeline.callbacks());

    Epoch epoch;
    auto next_check = std::chrono::steady_clock::now() + 100ms;
    auto last_input = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (backend.configuration_change_pending()) {
            epoch.restart = "device-format-change";
            break;
        }
        if (now >= next_check) {
            if (backend.configuration_changed()) {
                epoch.restart = "device-format-change";
                break;
            }
            next_check = now + 100ms;
        }
        const auto previous_measurements = echo_probe.levels.measured_frames;
        const auto batch = pipeline.process(backend.now());
        epoch.references += batch.reference_frames;
        epoch.inputs += batch.captures.size();
        epoch.input_silence += batch.input_silence_frames;
        epoch.reference_silence += batch.reference_silence_frames;
        if (!batch.captures.empty()) {
            last_input = now;
            if (!epoch.first) {
                epoch.first = batch.captures.front().timing;
            }
            epoch.last = batch.captures.back().timing;
            if (echo_enabled) {
                // Drain APM's one-entry statistics queue while processing is active.
                const auto delay = processor->estimated_echo_delay();
                if (echo_probe.levels.measured_frames > previous_measurements) {
                    epoch.apm_delay = delay;
                }
            }
        }
        if (batch.restart_required || now - last_input >= 500ms) {
            epoch.restart = batch.restart_required ? "audio-discontinuity" : "audio-stalled";
            break;
        }
        const auto wake = std::chrono::steady_clock::now() + 2ms;
        // The HAL delivers property-listener notifications on the main run loop.
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
        std::this_thread::sleep_until(wake);
    }
    backend.stop();
    epoch.input_peak = fixture.input_peak();
    epoch.echo = echo_probe.levels;

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
    if (epoch.echo.measured_frames > 0) {
        std::cout << "echo_frames=" << epoch.echo.measured_frames
                  << " quiet_rms=" << std::sqrt(epoch.echo.quiet.mean())
                  << " raw_rms=" << std::sqrt(epoch.echo.raw.mean())
                  << " clean_rms=" << std::sqrt(epoch.echo.clean.mean())
                  << " attenuation_db=" << epoch.echo.reduction_db();
        if (epoch.apm_delay) {
            std::cout << " apm_delay_ms=" << epoch.apm_delay->count();
        }
        std::cout << '\n';
    }
    return epoch;
}

DeviceFormats wait_for_devices(CoreAudioBackend& backend, CoreAudioOutput output,
                               CoreAudioInput input,
                               std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        try {
            return backend.open(output, input);
        } catch (const std::runtime_error&) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw;
            }
            std::this_thread::sleep_for(25ms);
        }
    }
}

enum class TestTransition { none, restart, switch_output };

int verify_webrtc(TestTransition transition, CoreAudioInput input_route, EchoCancellation echo)
{
    CoreAudioBackend backend;
    auto devices = backend.open(CoreAudioOutput::default_device, input_route);
    const auto initial_output = backend.output_device_id();
    const FrameFormat input{48000, devices.input.channels};
    const FrameFormat reference{48000, devices.output.channels};
    const unsigned phases = transition == TestTransition::none ? 1 : 2;
    std::uint64_t generation = 0;
    const bool echo_enabled = echo == EchoCancellation::enabled;
    for (unsigned phase = 0; phase < phases; ++phase) {
        const auto deadline = std::chrono::steady_clock::now() + (echo_enabled ? 5500ms : 1400ms);
        const auto output = phase == 1 && transition == TestTransition::switch_output
            ? CoreAudioOutput::system_device : CoreAudioOutput::default_device;
        std::size_t frames_processed = 0;
        bool recovered = false;
        bool captured_signal = false;
        Epoch epoch;
        do {
            if (++generation > 8) {
                throw std::runtime_error("Repeated audio interruption prevented verification");
            }
            if (generation > 1) {
                devices = wait_for_devices(backend, output, input_route, deadline);
            }
            if (output == CoreAudioOutput::system_device &&
                backend.output_device_id() == initial_output) {
                throw std::runtime_error("The default and system output are the same device");
            }
            epoch = run_epoch(backend, devices, input, reference, generation, deadline, echo);
            frames_processed += epoch.inputs;
            captured_signal = captured_signal || epoch.input_peak > 0;
            recovered = epoch.restart.empty();
        } while (!recovered && std::chrono::steady_clock::now() < deadline);
        if (!recovered || frames_processed < 10 || !captured_signal) {
            throw std::runtime_error("WebRTC did not receive a sustained run of complete frames");
        }
        if (echo_enabled) {
            if (epoch.echo.measured_frames < 100 || epoch.echo.quiet.samples == 0 || !epoch.apm_delay) {
                throw std::runtime_error("Too little settled playback to measure echo cancellation");
            }
            if (epoch.echo.raw.mean() <= 2.0 * epoch.echo.quiet.mean()) {
                throw std::runtime_error("Playback did not raise microphone power above the quiet baseline");
            }
            if (epoch.echo.reduction_db() < 6.0) {
                throw std::runtime_error("Echo cancellation reduced capture power by less than 6 dB");
            }
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
            const std::string_view command(argv[1]);
            if (command == "--verify-webrtc" || command == "--verify-echo") {
                auto transition = TestTransition::none;
                auto input = CoreAudioInput::default_device;
                for (int index = 2; index < argc; ++index) {
                    const std::string_view argument(argv[index]);
                    if (argument == "--built-in-mic" && input == CoreAudioInput::default_device) {
                        input = CoreAudioInput::built_in;
                    } else if (argument == "--restart" && transition == TestTransition::none) {
                        transition = TestTransition::restart;
                    } else if (argument == "--switch-output" && transition == TestTransition::none) {
                        transition = TestTransition::switch_output;
                    } else {
                        std::cerr << "Unknown or conflicting audio probe option: " << argument << '\n';
                        return 1;
                    }
                }
                const auto echo = command == "--verify-echo" ? EchoCancellation::enabled
                                                              : EchoCancellation::disabled;
                return verify_webrtc(transition, input, echo);
            }
            std::cerr << "Usage: voice_gate_audio_probe "
                         "[--verify-webrtc | --verify-echo] [--restart | --switch-output] "
                         "[--built-in-mic]\n";
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
            probe.output.total_frames < probe.playback_frames) {
            std::cerr << "Probe did not verify capture, playback, and ordered device timestamps.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
