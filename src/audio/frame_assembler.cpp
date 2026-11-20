#include "audio/frame_assembler.h"

#include "audio/pcm.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace voice_gate::audio {
namespace {

constexpr long double nanoseconds_per_second = 1'000'000'000;
constexpr std::size_t max_queued_windows = 32;

float mapped_sample(const QueuedBlock& block, std::size_t frame, unsigned channel,
                    unsigned target_channels)
{
    const auto native_channels = block.format.channels;
    const auto stride = block.format.bytes_per_sample();
    const auto* first = block.samples.data() + frame * native_channels * stride;
    if (native_channels == 1) {
        return read_sample(first, block.format.encoding);
    }
    if (target_channels == 1) {
        double sum = 0;
        for (unsigned native = 0; native < native_channels; ++native) {
            sum += read_sample(first + native * stride, block.format.encoding);
        }
        return static_cast<float>(sum / native_channels);
    }
    return channel < native_channels ? read_sample(first + channel * stride, block.format.encoding)
                                     : 0.0F;
}

}  // namespace

FrameAssembler::FrameAssembler(FrameFormat format)
    : format_(format), frame_size_(format.sample_rate / 100), channels_(format.channels)
{
    if (frame_size_ == 0 || format.sample_rate % 100 != 0 || channels_.empty()) {
        throw std::invalid_argument("Audio target format requires whole 10 ms frames and channels");
    }
}

void FrameAssembler::append_silence(std::size_t count)
{
    for (auto& channel : channels_) {
        channel.insert(channel.end(), count, 0.0F);
    }
    silence_.insert(silence_.end(), count, true);
    write_position_ += static_cast<std::int64_t>(count);
}

IngestResult FrameAssembler::ingest(const QueuedBlock& block)
{
    if (generation_ && block.generation < *generation_) {
        return IngestResult::late;
    }
    const auto source_stride = block.format.channels * block.format.bytes_per_sample();
    if (!block.first_sample || block.frames == 0 || source_stride == 0 ||
        !std::isfinite(block.format.sample_rate) || block.format.sample_rate <= 0 ||
        block.frames > block.samples.size() / source_stride ||
        block.frames * source_stride != block.samples.size()) {
        return IngestResult::restart_required;
    }

    const auto origin = origin_.value_or(*block.first_sample);
    const long double offset = (static_cast<long double>(block.first_sample->count()) -
                                origin.count()) * format_.sample_rate / nanoseconds_per_second;
    const long double duration = static_cast<long double>(block.frames) * format_.sample_rate /
                                 block.format.sample_rate;
    const auto limit = static_cast<long double>(std::numeric_limits<std::int64_t>::max() / 2);
    if (offset < -limit || offset > limit || duration > limit) {
        return IngestResult::restart_required;
    }
    const auto start = static_cast<std::int64_t>(std::llround(offset));
    const auto end = static_cast<std::int64_t>(std::llround(offset + duration));
    if (end <= write_position_) {
        return IngestResult::late;
    }
    const auto gap = std::max<std::int64_t>(0, start - write_position_);
    if (gap > static_cast<std::int64_t>(frame_size_) ||
        end - read_position_ > static_cast<std::int64_t>(max_queued_windows * frame_size_)) {
        return IngestResult::restart_required;
    }

    origin_ = origin;
    generation_ = block.generation;
    append_silence(static_cast<std::size_t>(gap));
    while (write_position_ < end) {
        const auto position = std::clamp(
            (write_position_ - offset) * block.format.sample_rate / format_.sample_rate,
            0.0L, static_cast<long double>(block.frames - 1));
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, block.frames - 1);
        const auto fraction = static_cast<float>(position - lower);
        for (unsigned channel = 0; channel < format_.channels; ++channel) {
            const auto first = mapped_sample(block, lower, channel, format_.channels);
            const auto next = mapped_sample(block, upper, channel, format_.channels);
            channels_[channel].push_back(std::lerp(first, next, fraction));
        }
        silence_.push_back(false);
        ++write_position_;
    }
    return IngestResult::accepted;
}

std::optional<AudioFrame> FrameAssembler::take_frame(bool pad_shortfall)
{
    if (silence_.empty()) {
        return std::nullopt;
    }
    if (silence_.size() < frame_size_) {
        if (!pad_shortfall) {
            return std::nullopt;
        }
        append_silence(frame_size_ - silence_.size());
    }
    AudioFrame frame{sample_time(read_position_), {}, 0};
    frame.samples.reserve(format_.channels);
    for (auto& channel : channels_) {
        const auto end = channel.begin() + static_cast<std::ptrdiff_t>(frame_size_);
        frame.samples.emplace_back(channel.begin(), end);
        channel.erase(channel.begin(), end);
    }
    const auto end = silence_.begin() + static_cast<std::ptrdiff_t>(frame_size_);
    frame.silence_frames = static_cast<std::size_t>(std::count(silence_.begin(), end, true));
    silence_.erase(silence_.begin(), end);
    read_position_ += static_cast<std::int64_t>(frame_size_);
    return frame;
}

std::optional<AudioTime> FrameAssembler::next_frame_start() const
{
    return origin_ && !silence_.empty() ? std::optional(sample_time(read_position_))
                                        : std::nullopt;
}

AudioTime FrameAssembler::sample_time(std::int64_t index) const
{
    return *origin_ + AudioTime(std::llround(index * nanoseconds_per_second /
                                           format_.sample_rate));
}

void FrameAssembler::reset()
{
    origin_.reset();
    generation_.reset();
    write_position_ = 0;
    read_position_ = 0;
    for (auto& channel : channels_) {
        channel.clear();
    }
    silence_.clear();
}

}  // namespace voice_gate::audio
