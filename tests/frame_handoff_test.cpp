#include "audio/frame_handoff.h"

#include <array>
#include <iostream>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

namespace {

class Recorder final : public AudioProcessor {
public:
    void analyze_reference(const AudioFrame&) override { calls.push_back('R'); }
    void set_stream_delay(std::chrono::milliseconds value) override
    {
        calls.push_back('D');
        delay = value;
    }
    void process_capture(AudioFrame&) override { calls.push_back('I'); }

    std::vector<char> calls;
    std::chrono::milliseconds delay{};
};

bool check(AudioTime reference_submission, AudioTime input_time, AudioTime input_submission,
           std::chrono::milliseconds expected, bool effects = true)
{
    Recorder recorder;
    const std::array times{reference_submission, input_submission};
    std::size_t next = 0;
    FrameHandoff handoff(recorder, [&] { return times.at(next++); }, effects);
    AudioFrame input{input_time, {}, 0};
    const auto result = handoff.submit(AudioFrame{106ms, {}, 0}, input);
    const auto expected_calls = effects ? std::vector{'R', 'D', 'I'} : std::vector{'R', 'I'};
    return result.reference_submission == reference_submission &&
           result.input_submission == input_submission &&
           result.buffering_delay() == expected && recorder.calls == expected_calls &&
           recorder.delay == (effects ? expected : 0ms);
}

}  // namespace

int main()
{
    if (!check(100ms, 130ms, 134ms, 10ms) || !check(102ms, 130ms, 134ms, 8ms) ||
        !check(100ms, 150ms, 154ms, 10ms) || !check(100ms, 130ms, 134ms, 10ms, false)) {
        std::cerr << "Expected reference-before-input order and buffering delay from only "
                     "the four recorded events.\n";
        return 1;
    }
}
