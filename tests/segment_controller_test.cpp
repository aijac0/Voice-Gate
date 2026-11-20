#include "segment/segment_controller.h"

#include <iostream>
#include <stdexcept>
#include <utility>

using namespace std::chrono_literals;
using namespace voice_gate::segment;
using voice_gate::audio::AudioFrame;
using voice_gate::audio::frame_duration;

namespace {

constexpr voice_gate::audio::FrameFormat format{48000, 1};
constexpr Activity idle{true, true, true};

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
T only(Actions actions)
{
    require(actions.size() == 1 && std::holds_alternative<T>(actions.front()),
            "Expected one action of the specified type");
    return std::get<T>(std::move(actions.front()));
}

Clock::time_point at(std::chrono::milliseconds timestamp)
{
    return Clock::time_point(timestamp);
}

Actions input(SegmentController& controller, std::chrono::milliseconds capture_time,
              bool loud, std::chrono::milliseconds processing_time)
{
    AudioFrame frame{capture_time, {std::vector<float>(480, loud ? 0.2F : 0.02F)}, 0};
    return controller.capture(std::move(frame), at(processing_time));
}

ControllerConfig config()
{
    return {.trigger = {0.1, 20ms}, .opening_timeout = 300ms, .finalization_timeout = 200ms,
            .buffered_audio_limit = 400ms, .store_segments = true, .pre_buffer = 100ms};
}

void check_capture_order(const Actions& actions, SegmentId id, std::chrono::milliseconds first,
                         std::chrono::milliseconds last)
{
    const auto count = static_cast<std::size_t>((last - first) / frame_duration + 1);
    require(actions.size() == count, "Expected every buffered frame exactly once");
    auto timestamp = first;
    for (const auto& action : actions) {
        const auto* audio = std::get_if<SendCapture>(&action);
        require(audio && audio->id == id && audio->frame.first_sample == timestamp,
                "Capture must be sent to the right segment in timestamp order");
        timestamp += frame_duration;
    }
}

void check_close_and_resume()
{
    SegmentController controller(format, config());
    for (auto capture_time = 0ms; capture_time < 100ms; capture_time += frame_duration) {
        require(input(controller, capture_time, false, capture_time + frame_duration).empty(),
                "Quiet capture never opens a segment");
    }
    require(input(controller, 100ms, true, 110ms).empty(), "Wait for confirmation");
    const auto first = only<OpenSegment>(input(controller, 110ms, true, 120ms));
    require(!first.resume_from && first.onset.first_loud_sample == 100ms &&
                first.onset.confirmed_through == 120ms && controller.phase() == Phase::opening,
            "Assign a fresh attempt ID before beginning the opening request");
    require(controller.segment_started(first.id, "peer-one", at(125ms)).empty(),
            "A started event alone must leave audio local");
    require(input(controller, 120ms, true, 130ms).empty(), "Hold capture while opening");
    const auto pre_roll = controller.opening_response(first.id, "peer-one", at(135ms));
    check_capture_order(pre_roll, first.id, 20ms, 120ms);
    std::cout << "First segment ready. Sent " << pre_roll.size()
              << " buffered frames from 20 ms through 120 ms.\n";
    require(controller.phase() == Phase::active &&
                controller.segment_started(first.id, "peer-one", at(136ms)).empty(),
            "A duplicate ready event must never flush audio twice");
    check_capture_order(input(controller, 130ms, false, 140ms), first.id, 130ms, 130ms);

    controller.report_activity(first.id, idle, at(200ms));
    require(controller.tick(at(18s)).empty(), "The 15-second minimum and 3-second grace both apply");
    controller.report_activity(first.id, {true, false, true}, at(18010ms));
    require(controller.tick(at(18200ms)).empty(), "Undrained playback blocks closure");
    controller.report_activity(first.id, {true, true, false}, at(18210ms));
    require(controller.tick(at(18990ms)).empty(), "Required work blocks closure");
    controller.report_activity(first.id, idle, at(19s));
    controller.report_activity(first.id + 1, {}, at(21s));

    check_capture_order(input(controller, 21990ms, true, 22s), first.id, 21990ms, 21990ms);
    require(controller.tick(at(22s)).empty(), "Speech observed before the close action keeps this segment");
    std::cout << "Speech at the close deadline kept the current segment open.\n";
    check_capture_order(input(controller, 22s, false, 22010ms), first.id, 22s, 22s);
    require(controller.tick(at(25009ms)).empty(), "Follow-up sound restarts the grace period");
    require(only<RequestClose>(controller.tick(at(25010ms))).id == first.id &&
                controller.phase() == Phase::finalizing,
            "Closure requests stop audio to the old segment");

    require(input(controller, 25010ms, true, 25020ms).empty() &&
                input(controller, 25020ms, true, 25030ms).empty(),
            "Speech after closure stays local until finalization");
    require(controller.segment_closed(first.id, "other-peer", true, at(25035ms)).empty(),
            "A closure event for another provider segment has no effect");
    const auto transition = controller.segment_closed(first.id, "peer-one", true, at(25040ms));
    require(transition.size() == 2, "Release the old segment before starting the new one");
    const auto& old = std::get<ReleaseSegment>(transition[0]);
    const auto& next = std::get<OpenSegment>(transition[1]);
    require(old.stored && old.reason == EndReason::closed && old.id == first.id &&
                next.id != old.id && next.resume_from == "peer-one",
            "The completed stored segment becomes the resume source");
    std::cout << "Closed the first segment and used its finalized ID for the next opening.\n";
    require(controller.segment_closed(first.id, "peer-one", true, at(25041ms)).empty(),
            "A late closure cannot modify the new attempt");
    require(input(controller, 25030ms, true, 25045ms).empty(), "Keep buffering through reopening");
    require(controller.opening_response(next.id, "peer-two", at(25050ms)).empty(),
            "The opening response alone must leave audio local");
    check_capture_order(controller.segment_started(next.id, "peer-two", at(25060ms)),
                        next.id, 25010ms, 25030ms);
    std::cout << "Second segment ready. Sent all three boundary frames before live input.\n";
    check_capture_order(input(controller, 25040ms, true, 25061ms), next.id, 25040ms, 25040ms);
}

void check_opening_failure_and_retry()
{
    auto options = config();
    options.pre_buffer = 30ms;
    options.buffered_audio_limit = 50ms;
    SegmentController controller(format, options, "saved");
    require(input(controller, 0ms, true, 10ms).empty(), "Enforce the confirmation interval");
    const auto first = only<OpenSegment>(input(controller, 10ms, true, 20ms));
    require(first.resume_from == "saved", "Use the configured finalized source");
    for (auto time = 20ms; time <= 40ms; time += frame_duration) {
        require(input(controller, time, false, time + frame_duration).empty(), "Retain pending audio");
    }
    const auto dropped = only<DropCapture>(input(controller, 50ms, false, 60ms));
    require(dropped.first_sample == 50ms && dropped.reason == CaptureDropReason::buffer_full,
            "Report the tail frame dropped at the pending buffer limit");
    require(controller.opening_response(first.id, "peer-one", at(319ms)).empty(),
            "Wait for the started event before the deadline");
    const auto expired = only<ReleaseSegment>(controller.segment_started(first.id, "peer-one", at(320ms)));
    require(expired.reason == EndReason::opening_timeout && expired.remote_id == "peer-one" &&
                controller.phase() == Phase::awaiting_retry,
            "An expired open reports the attempt and keeps buffered audio");

    const auto second = only<OpenSegment>(controller.retry(at(321ms), ResumePolicy::start_fresh));
    require(!second.resume_from && second.id > first.id, "A fresh retry uses a new attempt ID");
    require(controller.segment_started(first.id, "peer-one", at(322ms)).empty(),
            "Ignore ready events from a timed-out attempt");
    require(controller.opening_response(second.id, "peer-two", at(323ms)).empty(), "Wait for ready");
    require(only<ReleaseSegment>(controller.segment_started(second.id, "unexpected", at(324ms)))
                .reason == EndReason::opening_mismatch,
            "Peer mismatch cannot send pending microphone data");
    const auto third = only<OpenSegment>(controller.retry(at(325ms), ResumePolicy::start_fresh));
    require(controller.opening_response(third.id, "peer-three", at(326ms)).empty(), "Wait for ready");
    check_capture_order(controller.segment_started(third.id, "peer-three", at(327ms)), third.id, 0ms, 40ms);
    require(only<ReleaseSegment>(controller.transport_lost(third.id, at(328ms))).reason == EndReason::transport_lost &&
                controller.phase() == Phase::listening,
            "A lost active transport resumes listening without replaying sent capture");
}

void check_finalization_timeout()
{
    auto options = config();
    options.minimum_lifetime = 0ms;
    options.grace_period = 0ms;
    SegmentController controller(format, options, "saved");
    require(input(controller, 0ms, true, 10ms).empty(), "Enforce onset confirmation");
    const auto first = only<OpenSegment>(input(controller, 10ms, true, 20ms));
    require(controller.opening_response(first.id, "unconfirmed", at(21ms)).empty(), "Wait for ready");
    check_capture_order(controller.segment_started(first.id, "unconfirmed", at(22ms)), first.id, 0ms, 10ms);
    check_capture_order(input(controller, 20ms, false, 30ms), first.id, 20ms, 20ms);
    controller.report_activity(first.id, idle, at(31ms));
    require(only<RequestClose>(controller.tick(at(31ms))).id == first.id, "Request closure");
    require(input(controller, 30ms, true, 40ms).empty() && input(controller, 40ms, true, 50ms).empty(),
            "Capture after closure stays local");
    const auto result = only<ReleaseSegment>(controller.tick(at(231ms)));
    require(result.reason == EndReason::finalization_timeout && !result.stored &&
                controller.phase() == Phase::awaiting_retry,
            "An unconfirmed closure preserves the pending onset and releases the transport");
    require(controller.segment_closed(first.id, "unconfirmed", true, at(232ms)).empty(),
            "An expired closure cannot establish a new resume source");
    const auto second = only<OpenSegment>(controller.retry(at(233ms)));
    require(second.resume_from == "saved" && second.onset.first_loud_sample == 30ms,
            "Retry can use only a previously finalized source");
    (void)only<ReleaseSegment>(controller.transport_lost(second.id, at(234ms)));
    controller.discard_pending();
    require(controller.phase() == Phase::listening && controller.retry(at(235ms)).empty(),
            "An explicit discard clears the retained attempt");
}

void check_onset_across_confirmation()
{
    auto options = config();
    options.minimum_lifetime = 0ms;
    options.grace_period = 0ms;
    options.store_segments = false;
    SegmentController controller(format, options, "unused-source");
    require(input(controller, 0ms, true, 10ms).empty(), "Wait for a confirmed onset");
    const auto first = only<OpenSegment>(input(controller, 10ms, true, 20ms));
    require(!first.resume_from && controller.opening_response(first.id, "first", at(21ms)).empty(),
            "Storage disabled controls the opening route");
    check_capture_order(controller.segment_started(first.id, "first", at(22ms)), first.id, 0ms, 10ms);
    check_capture_order(input(controller, 20ms, false, 30ms), first.id, 20ms, 20ms);
    controller.report_activity(first.id, idle, at(31ms));
    (void)only<RequestClose>(controller.tick(at(31ms)));
    require(input(controller, 30ms, true, 40ms).empty(), "An unconfirmed onset stays local");
    const auto closed = only<ReleaseSegment>(controller.segment_closed(first.id, "first", true, at(41ms)));
    require(!closed.stored && controller.phase() == Phase::listening,
            "An idle closure releases the segment and keeps listening locally");
    const auto next = only<OpenSegment>(input(controller, 40ms, true, 50ms));
    require(!next.resume_from && next.onset.first_loud_sample == 30ms,
            "Confirmation can carry through the closure into a fresh opening");
    controller.capture_restarted();
    require(only<DropCapture>(input(controller, 40ms, true, 51ms)).reason == CaptureDropReason::out_of_order,
            "A new device run cannot replay a timestamp already seen by the controller");
    require(controller.opening_response(next.id, "second", at(52ms)).empty(), "Wait for ready");
    check_capture_order(controller.segment_started(next.id, "second", at(53ms)), next.id, 30ms, 40ms);
    require(only<ReleaseSegment>(controller.segment_closed(next.id, "second", false, at(54ms)))
                .reason == EndReason::remote_closed && controller.phase() == Phase::listening,
            "Provider-initiated finalization must release an active segment");
}

}  // namespace

int main()
{
    try {
        check_close_and_resume();
        check_opening_failure_and_retry();
        check_finalization_timeout();
        check_onset_across_confirmation();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
