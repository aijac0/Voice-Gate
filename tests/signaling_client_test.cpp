#include "transport/gpt_live.h"

#include <charconv>
#include <iostream>

using namespace std::chrono_literals;
using namespace voice_gate::transport;

namespace {

constexpr auto offer = "v=0\r\ns=cpp-client\r\n";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void check_signaling(std::uint16_t port)
{
    const GptLiveSignalingClient client(port, "test-client-token", 4s);
    const auto first = client.open(offer);
    const auto second = client.open(offer, std::string("live_saved"));
    require(first.session_id == "live_fresh" && first.sdp == "v=0\r\ns=fresh-answer\r\n" &&
                second.session_id == "live_resumed" && second.sdp == "v=0\r\ns=fork-answer\r\n",
            "Read the provider session ID and answer for fresh and resumed openings");

    try {
        (void)client.open(offer, std::string("live_unknown"));
        throw std::runtime_error("The uncertain provider response was accepted");
    } catch (const SignalingError& error) {
        require(error.status == 502 && error.provider_status == 503 && error.outcome_unknown,
                "Preserve the upstream status for an unknown opening outcome");
    }

    const GptLiveSignalingClient unauthorized(port, "wrong-token", 4s);
    try {
        (void)unauthorized.open(offer);
        throw std::runtime_error("The server accepted unauthorized signaling");
    } catch (const SignalingError& error) {
        require(error.status == 401 && !error.outcome_unknown,
                "An authentication rejection is definitive");
    }
}

void check_lifecycle_events()
{
    const auto started = parse_live_lifecycle(R"({"type":"session.started","session":{"id":"live_1","store":true}})");
    require(started && started->type == LiveLifecycleEvent::Type::started &&
                started->session_id == "live_1" && started->storage_enabled && !started->usage_seconds,
            "Expose the ready segment identity without creating a usage record");
    const auto closed = parse_live_lifecycle(R"({"type":"session.closed","session":{"id":"live_1","store":true},"reason":"close_requested","usage":{"seconds":16.5}})");
    require(closed && closed->type == LiveLifecycleEvent::Type::closed && closed->session_id == "live_1" &&
                closed->close_reason == "close_requested" && closed->usage_seconds == 16.5 &&
                closed->storage_enabled,
            "Preserve the terminal provider reason, usage, and storage configuration");
    require(!parse_live_lifecycle(R"({"type":"session.output_transcript.delta","delta":"test"})") &&
                !parse_live_lifecycle("not JSON"), "Only recognized lifecycle events can move the controller");
    try {
        (void)parse_live_lifecycle(R"({"type":"session.started","session":{"id":null}})");
        throw std::runtime_error("An invalid lifecycle identity was accepted");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "Pass the local test server port");
        const std::string_view argument(argv[1]);
        std::uint16_t port = 0;
        const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), port);
        require(result.ec == std::errc{} && result.ptr == argument.data() + argument.size(), "Invalid port");
        check_signaling(port);
        check_lifecycle_events();
        std::cout << "The native client exchanged fresh and resumed offers with the trusted server.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
