#include "transport/gpt_live.h"
#include "transport/webrtc_peer.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>

using namespace std::chrono_literals;
using namespace voice_gate::transport;

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint16_t port{8765};
    std::optional<std::string> resume_from;
    std::optional<std::string> bind_address;
};

void print_usage()
{
    std::cout << "Usage: voice_gate_live_probe --open-and-close [--port PORT] [--resume-from SESSION_ID]\n"
                 "       [--bind-address ADDRESS]\n"
                 "Run one GPT Live opening and closure through the local trusted server.\n"
                 "The probe sends signaling and control events and leaves microphone capture off.\n";
}

Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) {
            throw std::invalid_argument("A probe option is missing its value");
        }
        const std::string_view option(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (option == "--port") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.port);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.port == 0) {
                throw std::invalid_argument("Supply a port between 1 and 65535");
            }
        } else if (option == "--resume-from" && !value.empty()) {
            options.resume_from = std::string(value);
        } else if (option == "--bind-address" && !value.empty()) {
            options.bind_address = std::string(value);
        } else {
            throw std::invalid_argument("Unknown or empty live probe option");
        }
    }
    return options;
}

std::string gather_offer(WebRtcPeer& peer)
{
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        if (auto offer = peer.offer()) {
            return std::move(*offer);
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Timed out gathering the local WebRTC offer");
}

LiveLifecycleEvent wait_for_lifecycle(WebRtcPeer& peer, std::string_view id,
                                      LiveLifecycleEvent::Type expected,
                                      std::chrono::seconds duration)
{
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        const auto events = peer.take_events();
        if (events.dropped_control != 0) {
            throw std::runtime_error("The probe lost a GPT Live control event");
        }
        for (const auto& message : events.control) {
            if (auto event = parse_live_lifecycle(message)) {
                if (event->session_id != id) {
                    throw std::runtime_error("The provider event belongs to a different segment");
                }
                if (event->type == expected) {
                    return std::move(*event);
                }
                if (event->type == LiveLifecycleEvent::Type::closed) {
                    throw std::runtime_error("The provider closed the segment before readiness");
                }
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error(expected == LiveLifecycleEvent::Type::started
        ? "Timed out waiting for the provider to start the segment"
        : "Timed out waiting for closure confirmation and final usage");
}

void open_and_close(const Options& options)
{
    const auto* token = std::getenv("VOICE_GATE_CLIENT_TOKEN");
    if (!token) {
        throw std::invalid_argument("Set VOICE_GATE_CLIENT_TOKEN for the local trusted server");
    }
    const GptLiveSignalingClient signaling(options.port, token, 12s);
    const auto offer_start = Clock::now();
    WebRtcPeer peer({48000, 1}, {.ice_servers = {}, .bind_address = options.bind_address});
    const auto offer = gather_offer(peer);
    const auto offer_ready = Clock::now();
    const auto answer = signaling.open(offer, options.resume_from);
    const auto answer_received = Clock::now();
    peer.apply_answer(answer.sdp);
    (void)wait_for_lifecycle(peer, answer.session_id, LiveLifecycleEvent::Type::started, 12s);
    const auto close_request = Clock::now();
    peer.send_control(std::string(close_live_session));
    const auto closed = wait_for_lifecycle(peer, answer.session_id, LiveLifecycleEvent::Type::closed, 15s);
    const auto closure_received = Clock::now();
    const auto milliseconds = [](Clock::duration elapsed) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    };
    std::cout << "Segment " << closed.session_id << " closed.\n"
              << "Final voice usage: " << *closed.usage_seconds << " seconds\n"
              << "Closure reason: " << closed.close_reason << '\n'
              << "Storage configured: " << (closed.storage_enabled ? "yes" : "no") << '\n'
              << "Offer gathering " << milliseconds(offer_ready - offer_start) << " ms\n"
              << "Signaling response " << milliseconds(answer_received - offer_ready) << " ms\n"
              << "Startup confirmation " << milliseconds(close_request - answer_received) << " ms\n"
              << "Closure confirmation " << milliseconds(closure_received - close_request) << " ms\n";
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command != "--open-and-close") {
        print_usage();
        return argc == 1 || command == "--help" ? 0 : 1;
    }
    try {
        open_and_close(parse_options(argc, argv));
        return 0;
    } catch (const SignalingError& error) {
        std::cerr << error.what() << " (local status " << error.status;
        if (error.provider_status) {
            std::cerr << ", provider status " << *error.provider_status;
        }
        std::cerr << ")\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
