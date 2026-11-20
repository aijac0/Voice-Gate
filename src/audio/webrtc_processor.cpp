#include "audio/webrtc_processor.h"

#include <api/audio/audio_processing.h>

#include <stdexcept>
#include <string>

namespace voice_gate::audio {
namespace {

void check(int status, const char* operation)
{
    if (status != webrtc::AudioProcessing::kNoError) {
        throw std::runtime_error(std::string(operation) + " (WebRTC status " +
                                 std::to_string(status) + ")");
    }
}

void channel_pointers(const AudioFrame& frame, const webrtc::StreamConfig& config,
                      std::vector<const float*>& pointers)
{
    if (frame.samples.size() != config.num_channels()) {
        throw std::invalid_argument("Unexpected WebRTC channel count");
    }
    for (std::size_t channel = 0; channel < pointers.size(); ++channel) {
        if (frame.samples[channel].size() != config.num_frames()) {
            throw std::invalid_argument("Unexpected WebRTC frame size");
        }
        pointers[channel] = frame.samples[channel].data();
    }
}

class WebRtcProcessor final : public AudioProcessor {
public:
    WebRtcProcessor(FrameFormat input, FrameFormat reference)
        : processor_(webrtc::AudioProcessingBuilder().Create()),
          input_config_(static_cast<int>(input.sample_rate), input.channels),
          reference_config_(static_cast<int>(reference.sample_rate), reference.channels),
          input_channels_(input.channels), reference_channels_(reference.channels),
          output_(input.channels, std::vector<float>(input_config_.num_frames())),
          output_channels_(input.channels)
    {
        if (!processor_) {
            throw std::runtime_error("Create the WebRTC audio processor");
        }
        // The default configuration has the audio effects disabled.
        processor_->ApplyConfig(webrtc::AudioProcessing::Config{});
        webrtc::ProcessingConfig streams;
        streams.input_stream() = streams.output_stream() = input_config_;
        streams.reverse_input_stream() = streams.reverse_output_stream() = reference_config_;
        check(processor_->Initialize(streams), "Initialize WebRTC audio streams");
        for (std::size_t channel = 0; channel < output_.size(); ++channel) {
            output_channels_[channel] = output_[channel].data();
        }
    }

    void analyze_reference(const AudioFrame& frame) override
    {
        channel_pointers(frame, reference_config_, reference_channels_);
        check(processor_->AnalyzeReverseStream(reference_channels_.data(), reference_config_),
              "Submit the WebRTC audio output reference");
    }

    void set_stream_delay(std::chrono::milliseconds delay) override
    {
        check(processor_->set_stream_delay_ms(static_cast<int>(delay.count())),
              "Set the WebRTC buffering delay");
    }

    void process_capture(const AudioFrame& frame) override
    {
        channel_pointers(frame, input_config_, input_channels_);
        check(processor_->ProcessStream(input_channels_.data(), input_config_, input_config_,
                                        output_channels_.data()),
              "Submit WebRTC audio input");
    }

private:
    rtc::scoped_refptr<webrtc::AudioProcessing> processor_;
    webrtc::StreamConfig input_config_;
    webrtc::StreamConfig reference_config_;
    std::vector<const float*> input_channels_;
    std::vector<const float*> reference_channels_;
    std::vector<std::vector<float>> output_;
    std::vector<float*> output_channels_;
};

}  // namespace

std::unique_ptr<AudioProcessor> make_webrtc_processor(FrameFormat input, FrameFormat reference)
{
    return std::make_unique<WebRtcProcessor>(input, reference);
}

}  // namespace voice_gate::audio
