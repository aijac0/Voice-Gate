#include "segment/segment_controller.h"
#include "transport/gpt_live.h"
#include "transport/webrtc_peer.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using namespace voice_gate::segment;
using namespace voice_gate::transport;
using voice_gate::audio::AudioFrame;

namespace {

constexpr auto started_event = R"({"type":"session.started","session":{"id":"local-peer"}})";
constexpr auto closed_event = R"({"type":"session.closed","session":{"id":"local-peer"},"reason":"close_requested","usage":{"seconds":15}})";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void wait_for(Predicate predicate, const char* message)
{
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) {
        require(Clock::now() < deadline, message);
        std::this_thread::sleep_for(5ms);
    }
}

AudioFrame frame(std::chrono::milliseconds start, bool loud)
{
    std::vector<float> samples(480);
    if (loud) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto elapsed = std::chrono::duration<double>(start).count() + i / 48000.0;
            samples[i] = static_cast<float>(0.45 * std::sin(2.0 * std::numbers::pi * 440.0 * elapsed));
        }
    }
    return {start, {std::move(samples)}, 0};
}

struct ReceivedPacket {
    std::uint32_t timestamp;
    rtc::binary bytes;
};

struct ResponderState {
    std::mutex mutex;
    std::shared_ptr<rtc::DataChannel> channel;
    std::vector<std::string> messages;
    std::vector<ReceivedPacket> packets;
};

void check_loopback()
{
    SegmentController controller({48000, 1}, {
        .trigger = {0.1, 20ms}, .opening_timeout = 20s, .finalization_timeout = 10s,
        .buffered_audio_limit = 2s, .minimum_lifetime = 0ms, .grace_period = 0ms,
    });
    require(controller.capture(frame(0ms, false), Clock::now()).empty() &&
                controller.capture(frame(10ms, true), Clock::now()).empty(),
            "Quiet input and unconfirmed sound stay in the local pre-buffer");
    const auto actions = controller.capture(frame(20ms, true), Clock::now());
    require(actions.size() == 1 && std::holds_alternative<OpenSegment>(actions.front()),
            "A confirmed onset starts the test peer's opening");
    const auto id = std::get<OpenSegment>(actions.front()).id;

    WebRtcPeer client({48000, 1}, {.ice_servers = {}, .bind_address = "127.0.0.1"});
    auto state = std::make_shared<ResponderState>();
    const std::weak_ptr<ResponderState> destination = state;
    rtc::Configuration options;
    options.bindAddress = "127.0.0.1";
    options.disableAutoNegotiation = true;
    rtc::PeerConnection responder(options);

    constexpr std::uint32_t ssrc = 5196317;
    constexpr std::uint8_t payload_type = 111;
    rtc::Description::Audio description("audio", rtc::Description::Direction::SendRecv);
    description.addOpusCodec(payload_type);
    description.addSSRC(ssrc, "fixture", "fixture", "playback");
    auto track = responder.addTrack(description);
    const auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "fixture", payload_type, 48000);
    const auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
    packetizer->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
    track->setMediaHandler(packetizer);
    track->onFrame([destination](rtc::binary packet, rtc::FrameInfo info) {
        if (const auto target = destination.lock()) {
            std::lock_guard lock(target->mutex);
            target->packets.push_back({info.timestamp, std::move(packet)});
        }
    });
    responder.onDataChannel([destination](std::shared_ptr<rtc::DataChannel> channel) {
        if (const auto target = destination.lock()) {
            channel->onMessage([destination](rtc::message_variant message) {
                if (const auto queue = destination.lock()) {
                    if (auto* text = std::get_if<std::string>(&message)) {
                        std::lock_guard lock(queue->mutex);
                        queue->messages.push_back(std::move(*text));
                    }
                }
            });
            std::lock_guard lock(target->mutex);
            target->channel = std::move(channel);
        }
    });

    std::optional<std::string> offer;
    wait_for([&] { return (offer = client.offer()).has_value(); }, "Native offer gathering timed out");
    require(offer->find("a=fingerprint:") != std::string::npos &&
                offer->find("a=candidate:") != std::string::npos,
            "The offer must contain the DTLS fingerprint and gathered ICE candidates");
    responder.setRemoteDescription(rtc::Description(*offer, rtc::Description::Type::Offer));
    responder.setLocalDescription(rtc::Description::Type::Answer);
    wait_for([&] { return responder.gatheringState() == rtc::PeerConnection::GatheringState::Complete; },
             "Loopback answer gathering timed out");
    client.apply_answer(responder.localDescription()->generateSdp());
    require(controller.opening_response(id, "local-peer", Clock::now()).empty(),
            "Signaling alone must keep buffered microphone audio local");
    std::shared_ptr<rtc::DataChannel> control;
    wait_for([&] {
        std::lock_guard lock(state->mutex);
        control = state->channel;
        return client.connected() && track->isOpen() && control && control->isOpen();
    }, "Local WebRTC media and data channels did not connect");
    require(control->label() == "oai-events" && !client.send_capture(frame(0ms, false)),
            "Audio stays local until the controller opens the capture gate");
    control->send(started_event);
    std::optional<LiveLifecycleEvent> event;
    auto saw_event = [&](LiveLifecycleEvent::Type expected) {
        const auto events = client.take_events();
        for (const auto& message : events.control) {
            event = parse_live_lifecycle(message);
            if (event && event->type == expected) {
                return true;
            }
        }
        return false;
    };
    wait_for([&] { return saw_event(LiveLifecycleEvent::Type::started); },
             "The started event must reach the native client");

    const auto buffered = controller.segment_started(id, event->session_id, Clock::now());
    require(buffered.size() == 3 && controller.phase() == Phase::active, "Ready flushes the roll in order");
    client.allow_capture(true);
    for (const auto& action : buffered) {
        require(client.send_capture(std::get<SendCapture>(action).frame), "Send a buffered Opus frame");
    }
    const auto live = controller.capture(frame(30ms, true), Clock::now());
    require(live.size() == 1 && client.send_capture(std::get<SendCapture>(live.front()).frame),
            "Live Opus audio follows buffered capture");
    std::vector<ReceivedPacket> packets;
    wait_for([&] {
        std::lock_guard lock(state->mutex);
        if (state->packets.size() < 4) {
            return false;
        }
        packets.swap(state->packets);
        return true;
    }, "The loopback peer did not receive all four Opus packets");
    require(packets.size() == 4, "Expect exactly the buffered and live frames");
    for (std::size_t index = 0; index < packets.size(); ++index) {
        require(packets[index].timestamp - packets.front().timestamp == index * 480,
                "RTP timestamps must preserve the original 10 ms audio cadence");
        const auto elapsed = std::chrono::duration<double, std::milli>(10 * index);
        track->sendFrame(std::move(packets[index].bytes), elapsed);
    }

    std::vector<PlaybackAudio> playback;
    wait_for([&] {
        auto events = client.take_events();
        require(events.dropped_playback == 0, "Decode the returned packets without loss");
        for (auto& decoded : events.playback) {
            playback.push_back(std::move(decoded));
        }
        return playback.size() >= 4;
    }, "The client did not decode all four returned packets");
    require(playback.size() == 4 && playback.back().mono_samples.size() == 480 &&
                std::any_of(playback.back().mono_samples.begin(), playback.back().mono_samples.end(),
                            [](float sample) { return std::abs(sample) > 0.02F; }),
            "The Opus round trip must preserve a measurable signal in 10 ms mono output");

    const auto quiet = controller.capture(frame(40ms, false), Clock::now());
    require(quiet.size() == 1, "The active segment can accept the last quiet frame");
    controller.report_activity(id, {true, true, true}, Clock::now());
    const auto close = controller.tick(Clock::now());
    require(close.size() == 1 && std::holds_alternative<RequestClose>(close.front()),
            "The segment controller emits closure on the control channel");
    client.allow_capture(false);
    client.send_control(std::string(close_live_session));
    require(!client.send_capture(frame(50ms, true)), "Closure stops capture on the old peer");
    wait_for([&] {
        std::lock_guard lock(state->mutex);
        return state->messages.size() == 1 && state->messages.front() == close_live_session;
    }, "The loopback peer did not receive the closure command");
    control->send(closed_event);
    wait_for([&] { return saw_event(LiveLifecycleEvent::Type::closed); }, "The closure confirmation did not return");
    require(event->usage_seconds == 15.0, "The client must read final usage on the data channel");
    const auto release = controller.segment_closed(id, event->session_id, false, Clock::now());
    require(release.size() == 1 && std::holds_alternative<ReleaseSegment>(release.front()),
            "The matching confirmation releases the segment");
    responder.close();

    std::cout << "Local WebRTC connected. Four ordered Opus frames made the round trip. "
                 "Opening and closure events crossed the data channel.\n";
}

}  // namespace

int main()
{
    try {
        check_loopback();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
