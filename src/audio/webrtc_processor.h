#pragma once

#include "audio/frame_handoff.h"

#include <memory>

namespace voice_gate::audio {

enum class EchoCancellation { disabled, enabled };

[[nodiscard]] std::unique_ptr<AudioProcessor> make_webrtc_processor(
    FrameFormat input, FrameFormat reference, EchoCancellation echo = EchoCancellation::disabled);

}  // namespace voice_gate::audio
