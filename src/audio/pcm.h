#pragma once

#include "audio/audio_backend.h"

namespace voice_gate::audio {

[[nodiscard]] float read_sample(const std::byte* sample, SampleEncoding encoding) noexcept;
void write_sample(std::byte* destination, SampleEncoding encoding, float sample) noexcept;

}  // namespace voice_gate::audio
