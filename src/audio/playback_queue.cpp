#include "audio/playback_queue.h"

#include "audio/pcm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace voice_gate::audio {
namespace {

std::size_t slot_count(StreamFormat format, std::size_t capacity)
{
    const auto stride = format.bytes_per_sample();
    if (stride == 0 || format.channels == 0 || !std::isfinite(format.sample_rate) ||
        format.sample_rate <= 0.0 || capacity == 0 || capacity >= std::numeric_limits<std::size_t>::max() / stride) {
        throw std::invalid_argument("Invalid playback queue format or capacity");
    }
    return capacity + 1;
}

AudioTime::rep scheduled_end(const OutputBlock& block, std::size_t frames, double rate) noexcept
{
    constexpr auto unknown = std::numeric_limits<AudioTime::rep>::max();
    if (!block.first_sample) {
        return unknown;
    }
    const auto time = block.first_sample->count() + std::ceil(frames * 1'000'000'000.0L / rate);
    return time < unknown ? static_cast<AudioTime::rep>(time) : unknown;
}

}  // namespace

PlaybackQueue::PlaybackQueue(StreamFormat output, std::size_t capacity_frames)
    : output_(output), bytes_per_sample_(output.bytes_per_sample()),
      slots_(slot_count(output, capacity_frames)), samples_(slots_ * bytes_per_sample_)
{
}

std::size_t PlaybackQueue::advance(std::size_t index) const noexcept
{
    ++index;
    return index == slots_ ? 0 : index;
}

std::size_t PlaybackQueue::queued(std::size_t read, std::size_t write) const noexcept
{
    return write >= read ? write - read : (slots_ - read) + write;
}

bool PlaybackQueue::enqueue(std::span<const float> mono) noexcept
{
    auto write = write_.load(std::memory_order_relaxed);
    const auto read = read_.load(std::memory_order_acquire);
    if (mono.size() > slots_ - 1 - queued(read, write)) {
        return false;
    }
    for (const auto sample : mono) {
        write_sample(samples_.data() + write * bytes_per_sample_, output_.encoding, sample);
        write = advance(write);
    }
    write_.store(write, std::memory_order_release);
    return true;
}

bool PlaybackQueue::matches_layout(const OutputBlock& block) const noexcept
{
    if (block.frames == 0) {
        return false;
    }
    unsigned channels = 0;
    for (const auto& buffer : block.buffers) {
        if (buffer.channels == 0 || buffer.channels > output_.channels - channels ||
            buffer.bytes.size() % block.frames != 0 ||
            buffer.bytes.size() / block.frames / bytes_per_sample_ != buffer.channels ||
            buffer.bytes.size() / block.frames % bytes_per_sample_ != 0) {
            return false;
        }
        channels += buffer.channels;
    }
    return channels == output_.channels;
}

void PlaybackQueue::render(const OutputBlock& block) noexcept
{
    if (!matches_layout(block)) {
        for (const auto& buffer : block.buffers) {
            std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{});
        }
        return;
    }

    const auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    const auto played = std::min(block.frames, queued(read, write));
    auto next_read = read;
    for (const auto& buffer : block.buffers) {
        next_read = read;
        auto* destination = buffer.bytes.data();
        for (std::size_t frame = 0; frame < played; ++frame) {
            const auto* sample = samples_.data() + next_read * bytes_per_sample_;
            for (unsigned channel = 0; channel < buffer.channels; ++channel) {
                std::memcpy(destination, sample, bytes_per_sample_);
                destination += bytes_per_sample_;
            }
            next_read = advance(next_read);
        }
        std::fill(destination, buffer.bytes.data() + buffer.bytes.size(), std::byte{});
    }
    if (played != 0) {
        const auto end = scheduled_end(block, played, output_.sample_rate);
        scheduled_through_.store(std::max(end, scheduled_through_.load(std::memory_order_relaxed)),
                                 std::memory_order_relaxed);
        read_.store(next_read, std::memory_order_release);
    }
}

bool PlaybackQueue::drained(AudioTime now) const noexcept
{
    if (read_.load(std::memory_order_acquire) != write_.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto through = scheduled_through_.load(std::memory_order_relaxed);
    return through != std::numeric_limits<AudioTime::rep>::max() && now.count() >= through;
}

}  // namespace voice_gate::audio
