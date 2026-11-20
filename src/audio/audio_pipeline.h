#pragma once

#include "audio/frame_handoff.h"

namespace voice_gate::audio {

struct ProcessingBatch {
    std::size_t reference_frames{};
    std::size_t input_silence_frames{};
    std::size_t reference_silence_frames{};
    std::vector<FrameTiming> input_timings;
    bool restart_required{};
};

class AudioPipeline {
public:
    AudioPipeline(DeviceFormats devices, FrameFormat input, FrameFormat reference,
                  DeviceCallbacks source, AudioProcessor& processor,
                  std::function<AudioTime()> clock, std::uint64_t generation);

    [[nodiscard]] DeviceCallbacks callbacks() noexcept;
    [[nodiscard]] ProcessingBatch process(AudioTime now);

private:
    static void capture(void* context, const InputBlock& block) noexcept;
    static void render(void* context, const OutputBlock& block) noexcept;

    AudioQueue input_queue_;
    AudioQueue reference_queue_;
    FrameAssembler input_;
    FrameAssembler reference_;
    FrameHandoff handoff_;
    DeviceCallbacks source_;
    std::deque<ReferenceTiming> pending_references_;
    std::atomic<bool> queue_overflow_{false};
};

}  // namespace voice_gate::audio
