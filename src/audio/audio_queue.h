#pragma once

#include "audio/audio_backend.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace voice_gate::audio {

struct QueuedBlock {
    StreamFormat format;
    // Interleaved in device channel order. Individual sample bytes are unchanged.
    std::span<const std::byte> samples;
    std::size_t frames;
    std::optional<AudioTime> first_sample;
    std::uint64_t generation;
};

// One device callback produces blocks and one processing thread consumes them.
class AudioQueue {
public:
    AudioQueue(StreamFormat format, std::size_t max_frames, std::size_t capacity,
               std::uint64_t generation);

    [[nodiscard]] bool push(const InputBlock& block) noexcept;
    [[nodiscard]] bool push(const OutputBlock& block) noexcept;

    // The consumer holds the view until it calls pop().
    [[nodiscard]] std::optional<QueuedBlock> front() const noexcept;
    void pop() noexcept;

private:
    struct Slot {
        std::vector<std::byte> samples;
        std::size_t frames{};
        std::optional<AudioTime> first_sample;
    };

    template <typename Byte>
    bool push_block(const DeviceBlock<Byte>& block) noexcept;

    StreamFormat format_;
    std::size_t max_frames_;
    std::uint64_t generation_;
    std::vector<Slot> slots_;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
};

}  // namespace voice_gate::audio
