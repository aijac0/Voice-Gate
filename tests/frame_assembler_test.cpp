#include "audio/frame_assembler.h"

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

QueuedBlock block(const std::vector<float>& samples, unsigned rate, unsigned channels,
                  std::optional<AudioTime> time, std::uint64_t generation = 1)
{
    return {{static_cast<double>(rate), channels, SampleEncoding::float32},
            std::as_bytes(std::span(samples)), samples.size() / channels, time, generation};
}

void callback_boundaries()
{
    std::vector<float> samples(960);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(i) / samples.size();
    }
    FrameAssembler single({48000, 1});
    FrameAssembler split({48000, 1});
    const std::vector<float> first(samples.begin(), samples.begin() + 144);
    const std::vector<float> rest(samples.begin() + 144, samples.end());
    require(single.ingest(block(samples, 48000, 1, 100ms)) == IngestResult::accepted,
            "whole callback");
    require(split.ingest(block(first, 48000, 1, 100ms)) == IngestResult::accepted &&
                split.ingest(block(rest, 48000, 1, 103ms)) == IngestResult::accepted,
            "split callback");
    for (const auto time : {100ms, 110ms}) {
        const auto a = single.take_frame();
        const auto b = split.take_frame();
        require(a && b && a->first_sample == time && b->first_sample == time &&
                    a->samples == b->samples && a->samples.front().size() == 480,
                "callback boundary must preserve data and first-sample time");
    }
}

void format_change()
{
    FrameAssembler stereo({48000, 2});
    const std::vector<float> old_samples(192 * 2, 0.25F);
    const std::vector<float> new_samples(288, -0.5F);
    require(stereo.ingest(block(old_samples, 48000, 2, 100ms)) == IngestResult::accepted &&
                stereo.ingest(block(new_samples, 48000, 1, 104ms, 2)) == IngestResult::accepted &&
                stereo.ingest(block(old_samples, 48000, 2, 110ms)) == IngestResult::late,
            "format change and stale generation");
    const auto frame = stereo.take_frame();
    require(frame && frame->samples.size() == 2 && frame->samples[0] == frame->samples[1] &&
                frame->samples[0][191] == 0.25F && frame->samples[0][192] == -0.5F &&
                frame->silence_frames == 0,
            "new format must fill the same stereo target window");
    const std::vector<float> converted(441, 0.2F);
    require(stereo.ingest(block(converted, 44100, 1, 110ms, 2)) == IngestResult::accepted,
            "input resampling");
    const auto next = stereo.take_frame();
    require(next && next->first_sample == 110ms && next->samples[0].size() == 480 &&
                std::abs(next->samples[1].back() - 0.2F) < 0.00001F,
            "44.1 kHz input must produce a 48 kHz frame");
}

void gap_and_restart()
{
    FrameAssembler mono({48000, 1});
    const std::vector<float> half(240, 0.3F);
    require(mono.ingest(block(half, 48000, 1, 100ms)) == IngestResult::accepted &&
                !mono.take_frame() && mono.next_frame_start() == 100ms,
            "wait for the remaining half-frame");
    const auto padded = mono.take_frame(true);
    require(padded && padded->silence_frames == 240 && padded->samples[0][239] == 0.3F &&
                padded->samples[0][240] == 0.0F &&
                mono.ingest(block(half, 48000, 1, 105ms)) == IngestResult::late,
            "pad only the shortfall and discard samples that miss their deadline");
    require(mono.ingest(block(half, 48000, 1, 140ms)) == IngestResult::restart_required &&
                mono.ingest(block(half, 48000, 1, std::nullopt)) == IngestResult::restart_required,
            "a sustained gap or invalid timestamp requires restart");
    mono.reset();
    require(mono.ingest(block(half, 48000, 1, 160ms, 2)) == IngestResult::accepted &&
                mono.next_frame_start() == 160ms,
            "restart establishes a fresh time origin");
}

void normalize_pcm()
{
    FrameAssembler mono({48000, 1});
    std::vector<float> samples(480, 0.25F);
    samples[0] = 1.5F;
    samples[1] = -1.5F;
    samples[2] = std::numeric_limits<float>::quiet_NaN();
    require(mono.ingest(block(samples, 48000, 1, 100ms)) == IngestResult::accepted,
            "convert native float PCM");
    const auto frame = mono.take_frame();
    require(frame && frame->samples[0][0] == 1.0F && frame->samples[0][1] == -1.0F &&
                frame->samples[0][2] == 0.0F,
            "supply finite normalized PCM to WebRTC");
}

}  // namespace

int main()
{
    try {
        callback_boundaries();
        format_change();
        gap_and_restart();
        normalize_pcm();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
