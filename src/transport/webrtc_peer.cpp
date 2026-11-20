#include "transport/webrtc_peer.h"

#include <opus/opus.h>
#include <rtc/rtc.hpp>

#include <array>
#include <cmath>
#include <mutex>
#include <random>
#include <stdexcept>
#include <utility>

namespace voice_gate::transport {
namespace {

constexpr int sample_rate = 48000;
constexpr int frame_samples = sample_rate / 100;
constexpr int maximum_packet_samples = sample_rate * 120 / 1000;
constexpr std::size_t maximum_opus_bytes = 1275;
constexpr std::uint8_t opus_payload_type = 111;
constexpr std::size_t max_control_messages = 64;
constexpr std::size_t max_control_bytes = 64 * 1024;
constexpr std::size_t max_audio_packets = 256;

struct Packet {
    std::uint32_t timestamp;
    rtc::binary bytes;
};

struct Inbox {
    std::mutex mutex;
    std::vector<std::string> control;
    std::vector<Packet> audio;
    std::size_t dropped_control{};
    std::size_t dropped_audio{};
};

rtc::Configuration configuration(PeerOptions options)
{
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    config.bindAddress = std::move(options.bind_address);
    config.maxMessageSize = max_control_bytes;
    for (const auto& server : options.ice_servers) {
        config.iceServers.emplace_back(server);
    }
    return config;
}

using Encoder = std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)>;
using Decoder = std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)>;

Encoder make_encoder(audio::FrameFormat format)
{
    if (format.sample_rate != sample_rate || (format.channels != 1 && format.channels != 2)) {
        throw std::invalid_argument("WebRTC capture requires 48 kHz mono or stereo frames");
    }
    int status = OPUS_OK;
    Encoder encoder(opus_encoder_create(sample_rate, static_cast<int>(format.channels),
                                        OPUS_APPLICATION_VOIP, &status), opus_encoder_destroy);
    if (!encoder || status != OPUS_OK) {
        throw std::runtime_error("Create the Opus encoder");
    }
    return encoder;
}

Decoder make_decoder()
{
    int status = OPUS_OK;
    Decoder decoder(opus_decoder_create(sample_rate, 1, &status), opus_decoder_destroy);
    if (!decoder || status != OPUS_OK) {
        throw std::runtime_error("Create the Opus decoder");
    }
    return decoder;
}

}  // namespace

class WebRtcPeer::Impl {
public:
    Impl(audio::FrameFormat capture, PeerOptions options)
        : encoder(make_encoder(capture)), decoder(make_decoder()), channels(capture.channels),
          peer(configuration(std::move(options)))
    {
        std::uint32_t ssrc = std::random_device{}();
        if (ssrc == 0) {
            ssrc = 1;
        }
        const rtc::Description::Direction direction = rtc::Description::Direction::SendRecv;
        rtc::Description::Audio audio("audio", direction);
        audio.addOpusCodec(opus_payload_type);
        audio.addSSRC(ssrc, "voice-gate", "voice-gate", "capture");
        track = peer.addTrack(audio);
        auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "voice-gate", opus_payload_type,
                                                                rtc::OpusRtpPacketizer::DefaultClockRate);
        auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
        packetizer->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
        packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
        track->setMediaHandler(packetizer);

        channel = peer.createDataChannel("oai-events");
        const std::weak_ptr<Inbox> destination = inbox;
        channel->onMessage([destination](rtc::message_variant message) {
            if (const auto queue = destination.lock()) {
                std::lock_guard lock(queue->mutex);
                auto* text = std::get_if<std::string>(&message);
                if (!text || text->size() > max_control_bytes || queue->control.size() >= max_control_messages) {
                    ++queue->dropped_control;
                    return;
                }
                queue->control.push_back(std::move(*text));
            }
        });
        track->onFrame([destination](rtc::binary packet, rtc::FrameInfo info) {
            if (const auto queue = destination.lock()) {
                std::lock_guard lock(queue->mutex);
                if (packet.empty() || packet.size() > maximum_opus_bytes || queue->audio.size() >= max_audio_packets) {
                    ++queue->dropped_audio;
                    return;
                }
                queue->audio.push_back({info.timestamp, std::move(packet)});
            }
        });
        peer.setLocalDescription(rtc::Description::Type::Offer);
    }

    ~Impl() { peer.close(); }

    Encoder encoder;
    Decoder decoder;
    const unsigned channels;
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    rtc::PeerConnection peer;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> channel;
    bool capture_allowed{};
    std::optional<audio::AudioTime> origin;
    std::optional<audio::AudioTime> previous_frame;
};

WebRtcPeer::WebRtcPeer(audio::FrameFormat capture, PeerOptions options)
    : impl_(std::make_unique<Impl>(capture, std::move(options)))
{
}

WebRtcPeer::~WebRtcPeer() = default;

std::optional<std::string> WebRtcPeer::offer() const
{
    if (impl_->peer.gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
        if (const auto description = impl_->peer.localDescription()) {
            return description->generateSdp();
        }
    }
    return std::nullopt;
}

void WebRtcPeer::apply_answer(const std::string& sdp)
{
    impl_->peer.setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
}

bool WebRtcPeer::connected() const
{
    return impl_->track->isOpen() && impl_->channel->isOpen();
}

void WebRtcPeer::allow_capture(bool allowed) noexcept
{
    impl_->capture_allowed = allowed;
}

bool WebRtcPeer::send_capture(const audio::AudioFrame& frame)
{
    if (!impl_->capture_allowed || !connected()) {
        return false;
    }
    if (frame.samples.size() != impl_->channels ||
        (impl_->previous_frame && frame.first_sample <= *impl_->previous_frame)) {
        throw std::invalid_argument("Invalid layout or order for WebRTC capture");
    }
    std::array<float, frame_samples * 2> interleaved;
    for (unsigned channel = 0; channel < impl_->channels; ++channel) {
        if (frame.samples[channel].size() != frame_samples) {
            throw std::invalid_argument("WebRTC capture requires complete 10 ms frames");
        }
        for (std::size_t index = 0; index < frame_samples; ++index) {
            const auto sample = frame.samples[channel][index];
            if (!std::isfinite(sample)) {
                throw std::invalid_argument("WebRTC capture samples must be finite");
            }
            interleaved[index * impl_->channels + channel] = sample;
        }
    }

    std::array<unsigned char, maximum_opus_bytes> encoded;
    const auto size = opus_encode_float(impl_->encoder.get(), interleaved.data(), frame_samples,
                                        encoded.data(), static_cast<opus_int32>(encoded.size()));
    if (size < 0) {
        throw std::runtime_error("Encode a WebRTC capture frame");
    }
    const auto origin = impl_->origin.value_or(frame.first_sample);
    const auto elapsed = std::chrono::duration<double>(frame.first_sample - origin);
    impl_->track->sendFrame(reinterpret_cast<const std::byte*>(encoded.data()), size, elapsed);
    impl_->origin = origin;
    impl_->previous_frame = frame.first_sample;
    return true;
}

void WebRtcPeer::send_control(std::string message)
{
    if (!impl_->channel->isOpen() || message.size() > max_control_bytes) {
        throw std::runtime_error("WebRTC control channel is unavailable or the message is too large");
    }
    impl_->channel->send(std::move(message));
}

PeerEvents WebRtcPeer::take_events()
{
    PeerEvents events;
    std::vector<Packet> packets;
    {
        std::lock_guard lock(impl_->inbox->mutex);
        events.control.swap(impl_->inbox->control);
        packets.swap(impl_->inbox->audio);
        events.dropped_control = std::exchange(impl_->inbox->dropped_control, 0);
        events.dropped_playback = std::exchange(impl_->inbox->dropped_audio, 0);
    }
    events.playback.reserve(packets.size());
    std::array<float, maximum_packet_samples> decoded;
    for (const auto& packet : packets) {
        const auto samples = opus_decode_float(impl_->decoder.get(),
                                               reinterpret_cast<const unsigned char*>(packet.bytes.data()),
                                               static_cast<opus_int32>(packet.bytes.size()),
                                               decoded.data(), maximum_packet_samples, 0);
        if (samples <= 0) {
            ++events.dropped_playback;
            continue;
        }
        events.playback.push_back({packet.timestamp, {decoded.begin(), decoded.begin() + samples}});
    }
    return events;
}

}  // namespace voice_gate::transport
