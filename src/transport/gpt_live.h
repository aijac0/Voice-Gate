#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace voice_gate::transport {

struct OpeningAnswer {
    std::string session_id;
    std::string sdp;
};

class SignalingError : public std::runtime_error {
public:
    SignalingError(long status, bool outcome_unknown, std::optional<int> provider_status = {});

    const long status;
    const bool outcome_unknown;
    const std::optional<int> provider_status;
};

// A blocking local HTTP call. The app runs it on its signaling worker.
class GptLiveSignalingClient {
public:
    GptLiveSignalingClient(std::uint16_t port, std::string token, std::chrono::milliseconds timeout);

    [[nodiscard]] OpeningAnswer open(const std::string& offer,
                                     const std::optional<std::string>& resume_from = {}) const;

private:
    std::string url_;
    std::string token_;
    long timeout_ms_;
};

struct LiveLifecycleEvent {
    enum class Type { started, closed };

    Type type;
    std::string session_id;
    std::optional<double> usage_seconds;
    std::string close_reason;
    bool storage_enabled{};
};

inline constexpr std::string_view close_live_session = R"({"type":"session.close"})";

[[nodiscard]] std::optional<LiveLifecycleEvent> parse_live_lifecycle(std::string_view message);

}  // namespace voice_gate::transport
