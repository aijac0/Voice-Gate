#include "segment/segment_controller.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace voice_gate::segment {
namespace {

std::size_t buffered_frame_limit(const ControllerConfig& config)
{
    const auto count = config.buffered_audio_limit / audio::frame_duration;
    if (config.buffered_audio_limit < config.pre_buffer ||
        config.buffered_audio_limit % audio::frame_duration != std::chrono::milliseconds::zero() ||
        count <= 0 || !std::in_range<std::size_t>(count)) {
        throw std::invalid_argument("Buffered audio limit must cover the pre-buffer in whole 10 ms frames");
    }
    return static_cast<std::size_t>(count);
}

}  // namespace

SegmentController::SegmentController(audio::FrameFormat input, ControllerConfig config,
                                     std::optional<std::string> latest_finalized)
    : config_(config), buffered_frame_limit_(buffered_frame_limit(config)),
      trigger_(input, config.trigger), pre_buffer_(config.pre_buffer),
      latest_finalized_(config.store_segments ? std::move(latest_finalized) : std::nullopt)
{
    constexpr auto zero = std::chrono::milliseconds::zero();
    if (config.pre_buffer < config.trigger.confirmation || config.opening_timeout <= zero ||
        config.finalization_timeout <= zero || config.minimum_lifetime < zero || config.grace_period < zero) {
        throw std::invalid_argument("Invalid segment timing configuration");
    }
    if (latest_finalized_ && latest_finalized_->empty()) {
        throw std::invalid_argument("Stored resume source requires a remote segment ID");
    }
}

bool SegmentController::matches(SegmentId id) const noexcept
{
    return attempt_ && attempt_->id == id;
}

OpenSegment SegmentController::begin_open(Clock::time_point now, ResumePolicy resume)
{
    OpenSegment action{next_id_, resume == ResumePolicy::use_latest_finalized ? latest_finalized_
                                                                            : std::nullopt,
                       *pending_onset_};
    attempt_ = Attempt{next_id_++, std::nullopt, std::nullopt};
    deadline_ = now + config_.opening_timeout;
    phase_ = Phase::opening;
    return action;
}

Actions SegmentController::capture(audio::AudioFrame frame, Clock::time_point now)
{
    if (last_capture_ && frame.first_sample <= *last_capture_) {
        return {DropCapture{frame.first_sample, CaptureDropReason::out_of_order}};
    }
    const auto sound = trigger_.observe(frame);
    last_capture_ = frame.first_sample;
    local_sound_ = sound.sound_present;
    if (phase_ == Phase::active) {
        update_idle_timer(now);
        Actions actions;
        actions.emplace_back(SendCapture{attempt_->id, std::move(frame)});
        return actions;
    }

    if (!pending_onset_) {
        pre_buffer_.append(std::move(frame));
        if (!sound.onset) {
            return {};
        }
        pending_audio_ = pre_buffer_.take();
        pending_onset_ = sound.onset;
        if (phase_ == Phase::listening) {
            return {begin_open(now, ResumePolicy::use_latest_finalized)};
        }
        return {};
    }

    if (pending_audio_.size() == buffered_frame_limit_) {
        return {DropCapture{frame.first_sample, CaptureDropReason::buffer_full}};
    }
    pending_audio_.push_back(std::move(frame));
    return {};
}

void SegmentController::update_idle_timer(Clock::time_point now)
{
    if (activity_.turn_finished && activity_.playback_drained &&
        activity_.required_work_finished && !local_sound_) {
        idle_since_ = idle_since_.value_or(now);
    } else {
        idle_since_.reset();
    }
}

void SegmentController::report_activity(SegmentId id, Activity activity, Clock::time_point now)
{
    if (phase_ == Phase::active && matches(id)) {
        activity_ = activity;
        update_idle_timer(now);
    }
}

Actions SegmentController::tick(Clock::time_point now)
{
    if (phase_ == Phase::opening && now >= deadline_) {
        return finish(EndReason::opening_timeout, now);
    }
    if (phase_ == Phase::finalizing && now >= deadline_) {
        return finish(EndReason::finalization_timeout, now);
    }
    if (phase_ != Phase::active) {
        return {};
    }
    update_idle_timer(now);
    if (!idle_since_ || now < std::max(active_since_ + config_.minimum_lifetime, *idle_since_) +
                                config_.grace_period) {
        return {};
    }

    phase_ = Phase::finalizing;
    deadline_ = now + config_.finalization_timeout;
    pre_buffer_.clear();
    trigger_.reset();
    return {RequestClose{attempt_->id}};
}

Actions SegmentController::opening_response(SegmentId id, std::string remote_id, Clock::time_point now)
{
    return note_opening(OpeningEvent::response, id, std::move(remote_id), now);
}

Actions SegmentController::segment_started(SegmentId id, std::string remote_id, Clock::time_point now)
{
    return note_opening(OpeningEvent::started, id, std::move(remote_id), now);
}

Actions SegmentController::note_opening(OpeningEvent event, SegmentId id, std::string remote_id,
                                        Clock::time_point now)
{
    if (phase_ != Phase::opening || !matches(id)) {
        return {};
    }
    if (now >= deadline_) {
        return finish(EndReason::opening_timeout, now);
    }
    auto& slot = event == OpeningEvent::response ? attempt_->response_id : attempt_->started_id;
    if (remote_id.empty() || (slot && *slot != remote_id)) {
        return finish(EndReason::opening_mismatch, now);
    }
    slot = std::move(remote_id);
    if (!attempt_->response_id || !attempt_->started_id) {
        return {};
    }
    if (attempt_->response_id != attempt_->started_id) {
        return finish(EndReason::opening_mismatch, now);
    }

    Actions actions;
    actions.reserve(pending_audio_.size());
    while (!pending_audio_.empty()) {
        actions.emplace_back(SendCapture{id, std::move(pending_audio_.front())});
        pending_audio_.pop_front();
    }
    pending_onset_.reset();
    active_since_ = now;
    idle_since_.reset();
    activity_ = {};
    phase_ = Phase::active;
    return actions;
}

Actions SegmentController::segment_closed(SegmentId id, std::string_view remote_id, bool stored,
                                          Clock::time_point now)
{
    if (!matches(id) || attempt_->response_id != remote_id) {
        return {};
    }
    if (phase_ == Phase::active) {
        return finish(EndReason::remote_closed, now, stored && config_.store_segments);
    }
    if (phase_ != Phase::finalizing) {
        return {};
    }
    if (now >= deadline_) {
        return finish(EndReason::finalization_timeout, now);
    }
    return finish(EndReason::closed, now, stored && config_.store_segments);
}

Actions SegmentController::transport_lost(SegmentId id, Clock::time_point now)
{
    if (!matches(id)) {
        return {};
    }
    return finish(EndReason::transport_lost, now);
}

Actions SegmentController::finish(EndReason reason, Clock::time_point now, bool stored)
{
    Actions actions;
    auto remote_id = attempt_->response_id ? std::move(attempt_->response_id)
                                           : std::move(attempt_->started_id);
    if (stored) {
        latest_finalized_ = remote_id;
    }
    actions.emplace_back(ReleaseSegment{attempt_->id, std::move(remote_id), reason, stored});
    attempt_.reset();
    if (phase_ == Phase::active) {
        capture_restarted();
    }

    phase_ = pending_onset_ ? Phase::awaiting_retry : Phase::listening;
    if (reason == EndReason::closed && pending_onset_) {
        actions.emplace_back(begin_open(now, ResumePolicy::use_latest_finalized));
    }
    return actions;
}

Actions SegmentController::retry(Clock::time_point now, ResumePolicy resume)
{
    if (phase_ != Phase::awaiting_retry) {
        return {};
    }
    return {begin_open(now, resume)};
}

void SegmentController::capture_restarted() noexcept
{
    trigger_.reset();
    pre_buffer_.clear();
    local_sound_ = false;
    idle_since_.reset();
}

void SegmentController::discard_pending() noexcept
{
    if (phase_ == Phase::awaiting_retry) {
        pending_onset_.reset();
        pending_audio_.clear();
        capture_restarted();
        phase_ = Phase::listening;
    }
}

}  // namespace voice_gate::segment
