#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

namespace voice_gate::audio {

using AudioTime = std::chrono::nanoseconds;

enum class SampleEncoding { float32, signed16, signed32 };

struct StreamFormat {
    double sample_rate;
    unsigned channels;
    SampleEncoding encoding;

    [[nodiscard]] std::size_t bytes_per_sample() const noexcept;
};

struct DeviceFormats {
    StreamFormat input;
    StreamFormat output;
};

// Each buffer contains interleaved samples for its channels. A planar
// stream is represented by one single-channel buffer per channel.
template <typename Byte>
struct BufferView {
    std::span<Byte> bytes;
    unsigned channels;
};

template <typename Byte>
struct DeviceBlock {
    std::span<const BufferView<Byte>> buffers;
    std::size_t frames;
    std::optional<AudioTime> first_sample;
};

using InputBlock = DeviceBlock<const std::byte>;
using OutputBlock = DeviceBlock<std::byte>;

struct DeviceCallbacks {
    void* context;
    // Block memory belongs to the device and is valid only during the call.
    void (*capture)(void*, const InputBlock&) noexcept;
    void (*render)(void*, const OutputBlock&) noexcept;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    [[nodiscard]] virtual DeviceFormats open() = 0;
    virtual void start(DeviceCallbacks callbacks) = 0;
    virtual void stop() = 0;

    // Uses the same monotonic clock as the first-sample timestamps.
    [[nodiscard]] virtual AudioTime now() const noexcept = 0;
};

}  // namespace voice_gate::audio
