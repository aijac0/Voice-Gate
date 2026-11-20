#pragma once

#include "audio/frame_handoff.h"

#include <memory>

namespace voice_gate::audio {

[[nodiscard]] std::unique_ptr<AudioProcessor> make_webrtc_processor(FrameFormat input,
                                                                    FrameFormat reference);

}  // namespace voice_gate::audio
