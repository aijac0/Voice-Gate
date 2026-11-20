#include "audio/frame_handoff.h"

#include <array>
#include <iostream>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

namespace {

class Recorder final : public AudioProcessor {
public:
    explicit Recorder(bool echo_enabled) : echo_enabled(echo_enabled) {}

    bool echo_cancellation_enabled() const noexcept override { return echo_enabled; }
    void analyze_reference(const AudioFrame&) override { calls.push_back('R'); }
    void set_stream_delay(std::chrono::milliseconds value) override
    {
        calls.push_back('D');
        delay = value;
    }
    void process_capture(AudioFrame&) override { calls.push_back('I'); }

    std::vector<char> calls;
    std::chrono::milliseconds delay{};
    bool echo_enabled;
};

bool check(AudioTime reference_submission, AudioTime input_time, AudioTime input_submission,
           AudioTime expected, bool echo_enabled = true, AudioTime scheduled_output = 106ms)
{
    Recorder recorder(echo_enabled);
    const std::array times{reference_submission, input_submission};
    std::size_t next = 0;
    FrameHandoff handoff(recorder, [&] { return times.at(next++); });
    AudioFrame input{input_time, {}, 0};
    const auto result = handoff.submit(AudioFrame{scheduled_output, {}, 0}, input);
    const auto expected_calls = echo_enabled ? std::vector{'R', 'D', 'I'} : std::vector{'R', 'I'};
    const auto expected_delay = std::chrono::round<std::chrono::milliseconds>(expected);
    return result && result->reference_submission == reference_submission &&
           result->input_submission == input_submission && result->buffering_delay() == expected &&
           result->submitted_delay.has_value() == echo_enabled && recorder.calls == expected_calls &&
           recorder.delay == (echo_enabled ? expected_delay : 0ms);
}

bool rejects_timing(AudioTime scheduled_output, AudioTime capture_time, AudioTime input_submission)
{
    Recorder recorder(true);
    const std::array times{AudioTime(100ms), input_submission};
    std::size_t next = 0;
    FrameHandoff handoff(recorder, [&] { return times.at(next++); });
    AudioFrame input{capture_time, {}, 0};
    return !handoff.submit(AudioFrame{scheduled_output, {}, 0}, input) &&
           recorder.calls == std::vector{'R'};
}

}  // namespace

int main()
{
    if (!check(100ms, 130ms, 134ms, 10ms) || !check(102ms, 130ms, 134ms, 8ms) ||
        !check(100ms, 150ms, 154ms, 10ms) || !check(100ms, 130ms, 134ms, 10ms, false) ||
        !check(100ms, 130ms, 130ms, 0ms, true, 100ms) ||
        !check(100ms, 130ms, 140ms, 500ms, true, 590ms) ||
        !check(100ms, 130ms, 134ms, 10600us, true, 106600us)) {
        std::cerr << "Expected reference-before-input order and buffering delay from only "
                     "the four recorded events.\n";
        return 1;
    }
    if (!rejects_timing(99999us, 130ms, 130ms) || !rejects_timing(600001us, 130ms, 130ms) ||
        !rejects_timing(106ms, 130ms, 129ms) || !rejects_timing(106ms, 95ms, 99ms)) {
        std::cerr << "Out-of-range delay and invalid clock ordering must stop capture processing.\n";
        return 1;
    }
}
