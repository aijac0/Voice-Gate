#include "audio/audio_queue.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace voice_gate::audio {

AudioQueue::AudioQueue(StreamFormat format, std::size_t max_frames, std::size_t capacity,
                       std::uint64_t generation)
    : format_(format), max_frames_(max_frames), generation_(generation)
{
    const auto frame_bytes = format.channels * format.bytes_per_sample();
    if (frame_bytes == 0 || max_frames == 0 || capacity == 0 ||
        max_frames > std::numeric_limits<std::size_t>::max() / frame_bytes ||
        capacity >= slots_.max_size()) {
        throw std::invalid_argument("Invalid audio queue dimensions");
    }
    // The spare slot distinguishes full from empty without sharing a counter.
    slots_.resize(capacity + 1);
    for (auto& slot : slots_) {
        slot.samples.resize(max_frames * frame_bytes);
    }
}

template <typename Byte>
bool AudioQueue::push_block(const DeviceBlock<Byte>& block) noexcept
{
    const auto index = write_.load(std::memory_order_relaxed);
    const auto next = (index + 1) % slots_.size();
    if (next == read_.load(std::memory_order_acquire) ||
        block.frames == 0 || block.frames > max_frames_) {
        return false;
    }

    const auto sample_bytes = format_.bytes_per_sample();
    unsigned total_channels = 0;
    for (const auto& buffer : block.buffers) {
        if (buffer.channels == 0 || buffer.channels > format_.channels - total_channels ||
            buffer.bytes.size() != block.frames * buffer.channels * sample_bytes) {
            return false;
        }
        total_channels += buffer.channels;
    }
    if (total_channels != format_.channels) {
        return false;
    }

    auto& slot = slots_[index];
    if (block.buffers.size() == 1) {
        const auto bytes = block.buffers.front().bytes;
        std::memcpy(slot.samples.data(), bytes.data(), bytes.size());
    } else {
        const auto frame_bytes = format_.channels * sample_bytes;
        std::size_t offset = 0;
        for (const auto& buffer : block.buffers) {
            const auto group_bytes = buffer.channels * sample_bytes;
            for (std::size_t frame = 0; frame < block.frames; ++frame) {
                std::memcpy(slot.samples.data() + frame * frame_bytes + offset,
                            buffer.bytes.data() + frame * group_bytes, group_bytes);
            }
            offset += group_bytes;
        }
    }
    slot.frames = block.frames;
    slot.first_sample = block.first_sample;
    write_.store(next, std::memory_order_release);
    return true;
}

bool AudioQueue::push(const InputBlock& block) noexcept
{
    return push_block(block);
}

bool AudioQueue::push(const OutputBlock& block) noexcept
{
    return push_block(block);
}

std::optional<QueuedBlock> AudioQueue::front() const noexcept
{
    const auto index = read_.load(std::memory_order_relaxed);
    if (index == write_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto& slot = slots_[index];
    const auto bytes = slot.frames * format_.channels * format_.bytes_per_sample();
    return QueuedBlock{format_, {slot.samples.data(), bytes}, slot.frames,
                       slot.first_sample, generation_};
}

void AudioQueue::pop() noexcept
{
    const auto index = read_.load(std::memory_order_relaxed);
    if (index != write_.load(std::memory_order_acquire)) {
        read_.store((index + 1) % slots_.size(), std::memory_order_release);
    }
}

}  // namespace voice_gate::audio
