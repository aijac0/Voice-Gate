#include "audio/pcm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace voice_gate::audio {
namespace {

template <typename Value>
Value load(const std::byte* sample) noexcept
{
    Value value;
    std::memcpy(&value, sample, sizeof(value));
    return value;
}

template <typename Value>
void store(std::byte* destination, Value value) noexcept
{
    std::memcpy(destination, &value, sizeof(value));
}

}  // namespace

float read_sample(const std::byte* sample, SampleEncoding encoding) noexcept
{
    switch (encoding) {
    case SampleEncoding::float32: {
        const auto level = load<float>(sample);
        return std::isfinite(level) ? std::clamp(level, -1.0F, 1.0F) : 0.0F;
    }
    case SampleEncoding::signed16:
        return static_cast<float>(load<std::int16_t>(sample)) / 32768.0F;
    case SampleEncoding::signed32:
        return static_cast<float>(load<std::int32_t>(sample)) / 2147483648.0F;
    }
    return 0.0F;
}

void write_sample(std::byte* destination, SampleEncoding encoding, float sample) noexcept
{
    const double level = std::isfinite(sample) ? std::clamp(sample, -1.0F, 1.0F) : 0.0;
    switch (encoding) {
    case SampleEncoding::float32:
        store(destination, static_cast<float>(level));
        break;
    case SampleEncoding::signed16:
        store(destination, static_cast<std::int16_t>(
                               std::lround(level * (level < 0 ? 32768.0 : 32767.0))));
        break;
    case SampleEncoding::signed32:
        store(destination, static_cast<std::int32_t>(
                               std::llround(level * (level < 0 ? 2147483648.0 : 2147483647.0))));
        break;
    }
}

}  // namespace voice_gate::audio
