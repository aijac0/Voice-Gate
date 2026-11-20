#include "audio/local_activation.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace voice_gate::audio {
namespace {

std::size_t frames_in(std::chrono::milliseconds duration)
{
    const auto count = duration / frame_duration;
    if (count <= 0 || duration % frame_duration != std::chrono::milliseconds::zero() ||
        !std::in_range<std::size_t>(count)) {
        throw std::invalid_argument("Audio duration must be a positive multiple of 10 ms");
    }
    return static_cast<std::size_t>(count);
}

double peak_channel_rms(const AudioFrame& frame, FrameFormat format)
{
    const auto frames = format.sample_rate / 100;
    if (frame.samples.size() != format.channels || frame.silence_frames > frames) {
        throw std::invalid_argument("Unexpected activation frame layout");
    }
    double peak_power = 0.0;
    for (const auto& channel : frame.samples) {
        if (channel.size() != frames) {
            throw std::invalid_argument("Unexpected activation frame size");
        }
        double power = 0.0;
        for (double sample : channel) {
            power += sample * sample;
        }
        if (!std::isfinite(power)) {
            throw std::invalid_argument("Activation samples must be finite");
        }
        peak_power = std::max(peak_power, power);
    }
    return std::sqrt(peak_power / frames);
}

}  // namespace

SoundTrigger::SoundTrigger(FrameFormat format, SoundTriggerConfig config)
    : format_(format), frames_to_confirm_(frames_in(config.confirmation)),
      rms_threshold_(config.rms_threshold)
{
    if (format.channels == 0 || format.sample_rate == 0 || format.sample_rate % 100 != 0) {
        throw std::invalid_argument("Activation format requires whole 10 ms frames and channels");
    }
    if (!(rms_threshold_ > 0.0 && rms_threshold_ <= 1.0)) {
        throw std::invalid_argument("Activation RMS threshold must be greater than zero and at most one");
    }
}

SoundObservation SoundTrigger::observe(const AudioFrame& frame)
{
    if (previous_frame_ && frame.first_sample <= *previous_frame_) {
        throw std::invalid_argument("Activation requires increasing capture timestamps");
    }
    const auto rms = peak_channel_rms(frame, format_);
    SoundObservation observation{rms, frame.silence_frames == 0 && rms >= rms_threshold_, std::nullopt};
    if (previous_frame_ && frame.first_sample - *previous_frame_ != frame_duration) {
        loud_frames_ = 0;
    }
    previous_frame_ = frame.first_sample;

    if (!observation.sound_present) {
        loud_frames_ = 0;
        return observation;
    }
    if (loud_frames_ == 0) {
        first_loud_sample_ = frame.first_sample;
    }
    if (loud_frames_ < frames_to_confirm_) {
        ++loud_frames_;
        if (loud_frames_ == frames_to_confirm_) {
            observation.onset = SoundOnset{first_loud_sample_, frame.first_sample + frame_duration};
        }
    }
    return observation;
}

void SoundTrigger::reset() noexcept
{
    previous_frame_.reset();
    loud_frames_ = 0;
}

RollingPreBuffer::RollingPreBuffer(std::chrono::milliseconds duration) : capacity_(frames_in(duration))
{
}

void RollingPreBuffer::append(AudioFrame frame)
{
    if (!frames_.empty()) {
        const auto previous = frames_.back().first_sample;
        if (frame.first_sample <= previous) {
            throw std::invalid_argument("The rolling pre-buffer requires increasing capture timestamps");
        }
        if (frame.first_sample - previous != frame_duration) {
            frames_.clear();
        }
    }
    frames_.push_back(std::move(frame));
    if (frames_.size() > capacity_) {
        frames_.pop_front();
    }
}

std::deque<AudioFrame> RollingPreBuffer::take()
{
    return std::exchange(frames_, {});
}

void RollingPreBuffer::clear() noexcept
{
    frames_.clear();
}

}  // namespace voice_gate::audio
