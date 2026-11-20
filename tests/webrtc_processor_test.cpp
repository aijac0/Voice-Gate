#include "audio/webrtc_processor.h"

#include <array>
#include <iostream>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

int main()
{
    try {
        const auto processor = make_webrtc_processor({48000, 1}, {48000, 2});
        const AudioFrame reference{106ms, {std::vector<float>(480, 0.05F),
                                           std::vector<float>(480, -0.05F)}, 0};
        const AudioFrame input{130ms, {std::vector<float>(480, 0.02F)}, 0};
        const std::array<AudioTime, 2> times{100ms, 134ms};
        std::size_t next = 0;
        FrameHandoff handoff(*processor, [&] { return times.at(next++); }, false);
        return handoff.submit(reference, input).buffering_delay() == 10ms ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
