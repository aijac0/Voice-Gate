#pragma once

#include "audio/audio_backend.h"

#include <atomic>
#include <vector>

namespace voice_gate::audio {

// The producer queues mono samples already at the device rate. The output
// callback copies native PCM to every device channel and pads with silence.
class PlaybackQueue {
public:
    PlaybackQueue(StreamFormat output, std::size_t capacity_frames);

    [[nodiscard]] bool enqueue(std::span<const float> mono) noexcept;
    void render(const OutputBlock& block) noexcept;

    // Called by the producer using the device clock. Includes samples already
    // handed to the device that are scheduled to play in the future.
    [[nodiscard]] bool drained(AudioTime now) const noexcept;

private:
    [[nodiscard]] std::size_t advance(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t queued(std::size_t read, std::size_t write) const noexcept;
    [[nodiscard]] bool matches_layout(const OutputBlock& block) const noexcept;

    const StreamFormat output_;
    const std::size_t bytes_per_sample_;
    const std::size_t slots_;
    std::vector<std::byte> samples_;
    std::atomic<std::size_t> read_{0};
    std::atomic<std::size_t> write_{0};
    std::atomic<AudioTime::rep> scheduled_through_{0};
};

}  // namespace voice_gate::audio
