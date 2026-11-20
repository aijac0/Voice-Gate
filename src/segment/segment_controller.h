#pragma once

#include "audio/local_activation.h"

#include <string>
#include <string_view>
#include <variant>

namespace voice_gate::segment {

using SegmentId = std::uint64_t;
using Clock = std::chrono::steady_clock;

enum class Phase { listening, opening, active, finalizing, awaiting_retry };
enum class ResumePolicy { use_latest_finalized, start_fresh };
enum class EndReason {
    closed, remote_closed, opening_timeout, finalization_timeout, opening_mismatch, transport_lost
};
enum class CaptureDropReason { buffer_full, out_of_order };

struct ControllerConfig {
    audio::SoundTriggerConfig trigger;
    std::chrono::milliseconds opening_timeout;
    std::chrono::milliseconds finalization_timeout;
    std::chrono::milliseconds buffered_audio_limit;
    bool store_segments{};
    std::chrono::milliseconds pre_buffer{1000};
    std::chrono::milliseconds minimum_lifetime{15000};
    std::chrono::milliseconds grace_period{3000};
};

struct Activity {
    bool turn_finished{};
    bool playback_drained{};
    bool required_work_finished{};
};

struct OpenSegment {
    SegmentId id;
    std::optional<std::string> resume_from;
    audio::SoundOnset onset;
};

struct SendCapture {
    SegmentId id;
    audio::AudioFrame frame;
};

struct RequestClose {
    SegmentId id;
};

struct ReleaseSegment {
    SegmentId id;
    std::optional<std::string> remote_id;
    EndReason reason;
    bool stored;
};

struct DropCapture {
    audio::AudioTime first_sample;
    CaptureDropReason reason;
};

using Action = std::variant<OpenSegment, SendCapture, RequestClose, ReleaseSegment, DropCapture>;
using Actions = std::vector<Action>;

// The application serializes capture, transport events, and timer ticks. The
// returned actions must be handled in order on the same event loop.
class SegmentController {
public:
    SegmentController(audio::FrameFormat input, ControllerConfig config,
                      std::optional<std::string> latest_finalized = std::nullopt);

    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    [[nodiscard]] Actions capture(audio::AudioFrame frame, Clock::time_point now);
    [[nodiscard]] Actions tick(Clock::time_point now);
    [[nodiscard]] Actions opening_response(SegmentId id, std::string remote_id,
                                           Clock::time_point now);
    [[nodiscard]] Actions segment_started(SegmentId id, std::string remote_id,
                                          Clock::time_point now);
    [[nodiscard]] Actions segment_closed(SegmentId id, std::string_view remote_id, bool stored,
                                         Clock::time_point now);
    [[nodiscard]] Actions transport_lost(SegmentId id, Clock::time_point now);
    [[nodiscard]] Actions retry(Clock::time_point now,
                                 ResumePolicy resume = ResumePolicy::use_latest_finalized);
    void report_activity(SegmentId id, Activity activity, Clock::time_point now);
    void capture_restarted() noexcept;
    void discard_pending() noexcept;

private:
    enum class OpeningEvent { response, started };

    struct Attempt {
        SegmentId id;
        std::optional<std::string> response_id;
        std::optional<std::string> started_id;
    };

    [[nodiscard]] bool matches(SegmentId id) const noexcept;
    [[nodiscard]] OpenSegment begin_open(Clock::time_point now, ResumePolicy resume);
    [[nodiscard]] Actions note_opening(OpeningEvent event, SegmentId id, std::string remote_id,
                                       Clock::time_point now);
    [[nodiscard]] Actions finish(EndReason reason, Clock::time_point now, bool stored = false);
    void update_idle_timer(Clock::time_point now);

    const ControllerConfig config_;
    const std::size_t buffered_frame_limit_;
    audio::SoundTrigger trigger_;
    audio::RollingPreBuffer pre_buffer_;
    Phase phase_{Phase::listening};
    SegmentId next_id_{1};
    std::optional<Attempt> attempt_;
    std::optional<std::string> latest_finalized_;
    std::optional<audio::SoundOnset> pending_onset_;
    std::deque<audio::AudioFrame> pending_audio_;
    std::optional<audio::AudioTime> last_capture_;
    Clock::time_point deadline_{};
    Clock::time_point active_since_{};
    std::optional<Clock::time_point> idle_since_;
    Activity activity_;
    bool local_sound_{};
};

}  // namespace voice_gate::segment
