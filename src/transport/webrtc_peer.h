#pragma once

#include "audio/frame_assembler.h"

#include <memory>
#include <string>

namespace voice_gate::transport {

struct PeerOptions {
    std::vector<std::string> ice_servers;
    std::optional<std::string> bind_address;
};

struct PlaybackAudio {
    std::uint32_t rtp_timestamp;
    std::vector<float> mono_samples;
};

struct PeerEvents {
    std::vector<std::string> control;
    std::vector<PlaybackAudio> playback;
    std::size_t dropped_control{};
    std::size_t dropped_playback{};
};

// The caller owns this peer on the same event loop as the segment controller.
// Network callbacks enqueue bounded messages for that loop to retrieve.
class WebRtcPeer {
public:
    WebRtcPeer(audio::FrameFormat capture, PeerOptions options = {});
    ~WebRtcPeer();

    [[nodiscard]] std::optional<std::string> offer() const;
    void apply_answer(const std::string& sdp);
    [[nodiscard]] bool connected() const;
    void allow_capture(bool allowed) noexcept;
    [[nodiscard]] bool send_capture(const audio::AudioFrame& frame);
    void send_control(std::string message);
    [[nodiscard]] PeerEvents take_events();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace voice_gate::transport
