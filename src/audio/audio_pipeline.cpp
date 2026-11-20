#include "audio/audio_pipeline.h"

#include <utility>

namespace voice_gate::audio {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t max_device_blocks = 32;
constexpr std::size_t max_pending_reference_frames = 32;
// The first Mac probe observed ~31 ms from capture to callback. Allow two
// additional typical callbacks before declaring a partial frame late.
constexpr auto frame_deadline = 60ms;

bool ingest_blocks(AudioQueue& queue, FrameAssembler& assembler)
{
    while (const auto block = queue.front()) {
        const auto result = assembler.ingest(*block);
        queue.pop();
        if (result == IngestResult::restart_required) {
            return false;
        }
    }
    return true;
}

std::optional<AudioFrame> take_due_frame(FrameAssembler& assembler, AudioTime now)
{
    const auto start = assembler.next_frame_start();
    return assembler.take_frame(start && now >= *start + frame_deadline);
}

}  // namespace

AudioPipeline::AudioPipeline(DeviceFormats devices, FrameFormat input, FrameFormat reference,
                             DeviceCallbacks source, AudioProcessor& processor,
                             std::function<AudioTime()> clock, std::uint64_t generation)
    : input_queue_(devices.input, devices.max_input_frames, max_device_blocks, generation),
      reference_queue_(devices.output, devices.max_output_frames, max_device_blocks, generation),
      input_(input), reference_(reference), handoff_(processor, std::move(clock), false),
      source_(source)
{
}

DeviceCallbacks AudioPipeline::callbacks() noexcept
{
    return {this, capture, render};
}

void AudioPipeline::capture(void* context, const InputBlock& block) noexcept
{
    auto& pipeline = *static_cast<AudioPipeline*>(context);
    if (!pipeline.input_queue_.push(block)) {
        pipeline.queue_overflow_.store(true, std::memory_order_release);
    }
    if (pipeline.source_.capture) {
        pipeline.source_.capture(pipeline.source_.context, block);
    }
}

void AudioPipeline::render(void* context, const OutputBlock& block) noexcept
{
    auto& pipeline = *static_cast<AudioPipeline*>(context);
    if (pipeline.source_.render) {
        pipeline.source_.render(pipeline.source_.context, block);
    }
    if (!pipeline.reference_queue_.push(block)) {
        pipeline.queue_overflow_.store(true, std::memory_order_release);
    }
}

ProcessingBatch AudioPipeline::process(AudioTime now)
{
    ProcessingBatch batch;
    if (queue_overflow_.load(std::memory_order_acquire) ||
        !ingest_blocks(reference_queue_, reference_) ||
        !ingest_blocks(input_queue_, input_)) {
        batch.restart_required = true;
        return batch;
    }
    while (auto reference = take_due_frame(reference_, now)) {
        if (pending_references_.size() == max_pending_reference_frames) {
            batch.restart_required = true;
            return batch;
        }
        pending_references_.push_back(handoff_.submit_reference(*reference));
        batch.reference_silence_frames += reference->silence_frames;
        ++batch.reference_frames;
    }
    while (!pending_references_.empty()) {
        const auto input = take_due_frame(input_, now);
        if (!input) {
            break;
        }
        batch.input_timings.push_back(handoff_.submit_input(*input, pending_references_.front()));
        pending_references_.pop_front();
        batch.input_silence_frames += input->silence_frames;
    }
    return batch;
}

}  // namespace voice_gate::audio
