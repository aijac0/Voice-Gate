#include "transport/gpt_live.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace voice_gate::transport {
namespace {

using Json = nlohmann::json;
using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;
constexpr std::size_t max_response_bytes = 256 * 1024;

struct CurlGlobal {
    CurlGlobal()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Initialize HTTP signaling");
        }
    }

    ~CurlGlobal() { curl_global_cleanup(); }
};

void add_header(Headers& headers, const std::string& value)
{
    auto* list = curl_slist_append(headers.get(), value.c_str());
    if (!list) {
        throw std::runtime_error("Allocate HTTP signaling headers");
    }
    (void)headers.release();
    headers.reset(list);
}

template <typename Value>
void set_option(CURL* handle, CURLoption option, Value value)
{
    if (curl_easy_setopt(handle, option, value) != CURLE_OK) {
        throw std::runtime_error("Configure HTTP signaling");
    }
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* destination) noexcept
{
    auto& body = *static_cast<std::string*>(destination);
    if (size == 0 || count > (max_response_bytes - body.size()) / size) {
        return 0;
    }
    try {
        body.append(data, size * count);
        return size * count;
    } catch (...) {
        return 0;
    }
}

long validate_timeout(std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero() || !std::in_range<long>(timeout.count())) {
        throw std::invalid_argument("Invalid HTTP signaling timeout");
    }
    return static_cast<long>(timeout.count());
}

const Json& required_string(const Json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->get_ref<const std::string&>().empty()) {
        throw std::invalid_argument("Missing required GPT Live field");
    }
    return *found;
}

}  // namespace

SignalingError::SignalingError(long status, bool outcome_unknown, std::optional<int> provider_status)
    : std::runtime_error(outcome_unknown ? "The GPT Live opening outcome is unknown"
                                        : "The GPT Live opening was rejected"),
      status(status), outcome_unknown(outcome_unknown), provider_status(provider_status)
{
}

GptLiveSignalingClient::GptLiveSignalingClient(std::uint16_t port, std::string token,
                                             std::chrono::milliseconds timeout)
    : url_("http://127.0.0.1:" + std::to_string(port) + "/segments/open"),
      token_(std::move(token)), timeout_ms_(validate_timeout(timeout))
{
    if (port == 0 || token_.empty() || std::any_of(token_.begin(), token_.end(), [](unsigned char value) {
        return value < 33 || value > 126;
    })) {
        throw std::invalid_argument("Local signaling requires a valid port and ASCII client token");
    }
}

OpeningAnswer GptLiveSignalingClient::open(const std::string& offer,
                                         const std::optional<std::string>& resume_from) const
{
    static const CurlGlobal global;
    Json outgoing = {{"sdp", offer}};
    if (resume_from) {
        outgoing["resume_from"] = *resume_from;
    }
    const auto request = outgoing.dump();
    Headers headers(nullptr, curl_slist_free_all);
    add_header(headers, "Content-Type: application/json");
    add_header(headers, "Authorization: Bearer " + token_);

    std::string response;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) {
        throw std::runtime_error("Create HTTP signaling request");
    }
    set_option(handle.get(), CURLOPT_URL, url_.c_str());
    set_option(handle.get(), CURLOPT_PROXY, "");
    set_option(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    set_option(handle.get(), CURLOPT_NOSIGNAL, 1L);
    set_option(handle.get(), CURLOPT_TIMEOUT_MS, timeout_ms_);
    set_option(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    set_option(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.size()));
    set_option(handle.get(), CURLOPT_POSTFIELDS, request.data());
    set_option(handle.get(), CURLOPT_WRITEFUNCTION, &append_body);
    set_option(handle.get(), CURLOPT_WRITEDATA, static_cast<void*>(&response));
    if (curl_easy_perform(handle.get()) != CURLE_OK) {
        throw SignalingError(0, true);
    }
    long status = 0;
    if (curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) {
        throw SignalingError(0, true);
    }

    const auto json = Json::parse(response, nullptr, false);
    if (status != 201) {
        const auto reason = json.find("error");
        const auto upstream = json.find("provider_status");
        std::optional<int> provider_status;
        if (upstream != json.end() && upstream->is_number_integer() && *upstream >= 100 && *upstream <= 599) {
            provider_status = upstream->get<int>();
        }
        const bool unknown = reason == json.end() ||
                             *reason == "provider_outcome_unknown" ||
                             (status >= 500 && *reason != "provider_rejected");
        throw SignalingError(status, unknown, provider_status);
    }
    try {
        return {required_string(json, "session_id").get<std::string>(),
                required_string(json, "sdp").get<std::string>()};
    } catch (const std::invalid_argument&) {
        throw SignalingError(status, true);
    }
}

std::optional<LiveLifecycleEvent> parse_live_lifecycle(std::string_view message)
{
    const auto json = Json::parse(message, nullptr, false);
    const auto type = json.find("type");
    if (type == json.end() || (*type != "session.started" && *type != "session.closed")) {
        return std::nullopt;
    }
    const auto session = json.find("session");
    if (session == json.end()) {
        throw std::invalid_argument("GPT Live lifecycle event requires a session snapshot");
    }
    const auto id = required_string(*session, "id").get<std::string>();
    const auto store = session->find("store");
    const bool storage_enabled = store != session->end() && *store == true;
    if (*type == "session.started") {
        return LiveLifecycleEvent{LiveLifecycleEvent::Type::started, id, {}, {}, storage_enabled};
    }

    const auto usage = json.find("usage");
    if (usage == json.end()) {
        throw std::invalid_argument("GPT Live closure event requires final usage");
    }
    const auto seconds = usage->find("seconds");
    if (seconds == usage->end() || !seconds->is_number() || *seconds < 0 || !std::isfinite(seconds->get<double>())) {
        throw std::invalid_argument("Invalid GPT Live voice usage");
    }
    return LiveLifecycleEvent{LiveLifecycleEvent::Type::closed, id, seconds->get<double>(),
                              required_string(json, "reason").get<std::string>(), storage_enabled};
}

}  // namespace voice_gate::transport
