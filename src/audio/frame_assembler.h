#pragma once

#include "audio/audio_queue.h"

#include <deque>

namespace voice_gate::audio {

struct FrameFormat {
    unsigned sample_rate;
    unsigned channels;
};

struct AudioFrame {
    AudioTime first_sample;
    std::vector<std::vector<float>> samples;
    std::size_t silence_frames;
};

enum class IngestResult { accepted, late, restart_required };

// Converts independently timed device blocks into contiguous 10 ms windows.
// The target format is fixed for the lifetime of the assembler.
class FrameAssembler {
public:
    explicit FrameAssembler(FrameFormat format);

    [[nodiscard]] IngestResult ingest(const QueuedBlock& block);
    [[nodiscard]] std::optional<AudioFrame> take_frame(bool pad_shortfall = false);
    [[nodiscard]] std::optional<AudioTime> next_frame_start() const;
    void reset();

private:
    void append_silence(std::size_t count);
    [[nodiscard]] AudioTime sample_time(std::int64_t index) const;

    FrameFormat format_;
    std::size_t frame_size_;
    std::optional<AudioTime> origin_;
    std::optional<std::uint64_t> generation_;
    std::int64_t write_position_{};
    std::int64_t read_position_{};
    std::vector<std::deque<float>> channels_;
    std::deque<bool> silence_;
};

}  // namespace voice_gate::audio
