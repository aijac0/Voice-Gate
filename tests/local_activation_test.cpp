#include "audio/local_activation.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace voice_gate::audio;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AudioFrame make_frame(AudioTime time, float right_level, std::size_t padding = 0)
{
    return {time, {std::vector<float>(480, 0.02F), std::vector<float>(480, right_level)}, padding};
}

void check_sound_confirmation()
{
    SoundTrigger detector({48000, 2}, {0.1, 30ms});
    require(!detector.observe(make_frame(0ms, 0.02F)).sound_present, "Quiet input stays local");

    const auto loud = detector.observe(make_frame(10ms, -0.2F));
    require(loud.sound_present && std::abs(loud.rms - 0.2) < 0.00001 && !loud.onset,
            "The loudest channel qualifies, even when its samples are negative");
    require(!detector.observe(make_frame(20ms, 0.2F)).onset, "A short transient is insufficient");
    require(!detector.observe(make_frame(30ms, 0.2F, 1)).sound_present,
            "Padded frames cannot contribute to confirmation");
    require(!detector.observe(make_frame(40ms, 0.2F)).onset, "Padding restarts confirmation");
    require(!detector.observe(make_frame(60ms, 0.2F)).onset, "A missing frame restarts confirmation");
    require(!detector.observe(make_frame(70ms, 0.2F)).onset, "Confirmation still needs one frame");
    const auto confirmed = detector.observe(make_frame(80ms, -0.2F)).onset;
    require(confirmed && confirmed->first_loud_sample == 60ms && confirmed->confirmed_through == 90ms,
            "Keep the first qualifying timestamp and the end of the confirming frame");
    require(!detector.observe(make_frame(90ms, 0.2F)).onset,
            "A sustained burst produces one opening event");
    require(!detector.observe(make_frame(100ms, 0.0F)).sound_present, "Quiet input re-arms the trigger");
    require(!detector.observe(make_frame(110ms, 0.2F)).onset &&
                !detector.observe(make_frame(120ms, 0.2F)).onset &&
                detector.observe(make_frame(130ms, 0.2F)).onset.has_value(),
            "A second burst can trigger after a quiet frame");
    detector.reset();
    require(!detector.observe(make_frame(0ms, 0.2F)).onset,
            "Device recovery discards timestamps and confirmation from the old run");
}

void check_pre_buffer()
{
    RollingPreBuffer buffer;
    for (auto time = 0ms; time <= 1200ms; time += frame_duration) {
        buffer.append(make_frame(time, 0.0F));
    }
    auto frames = buffer.take();
    require(frames.size() == 100 && frames.front().first_sample == 210ms &&
                frames.back().first_sample == 1200ms && buffer.take().empty(),
            "Transfer the latest second in order exactly once");

    buffer.append(make_frame(1210ms, 0.0F));
    buffer.append(make_frame(1220ms, 0.0F, 10));
    frames = buffer.take();
    require(frames.size() == 2 && frames.back().silence_frames == 10,
            "Padding preserves the pre-buffer timeline");

    buffer.append(make_frame(1230ms, 0.0F));
    buffer.append(make_frame(1300ms, 0.2F));
    frames = buffer.take();
    require(frames.size() == 1 && frames.front().first_sample == 1300ms,
            "A discontinuity cannot join old and new input in the pre-buffer");
    buffer.append(make_frame(1310ms, 0.2F));
    buffer.clear();
    require(buffer.take().empty(), "Discard old buffered audio on device restart");
}

void check_invalid_input()
{
    SoundTrigger trigger({48000, 2}, {0.1, 10ms});
    try {
        (void)trigger.observe(make_frame(0ms, std::numeric_limits<float>::quiet_NaN()));
        throw std::runtime_error("Nonfinite audio was accepted");
    } catch (const std::invalid_argument&) {
    }
    require(trigger.observe(make_frame(0ms, 0.2F)).onset.has_value(),
            "Invalid samples must not consume the frame timestamp");
    try {
        (void)trigger.observe(make_frame(0ms, 0.2F));
        throw std::runtime_error("A duplicate capture timestamp was accepted");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main()
{
    try {
        check_sound_confirmation();
        check_pre_buffer();
        check_invalid_input();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
