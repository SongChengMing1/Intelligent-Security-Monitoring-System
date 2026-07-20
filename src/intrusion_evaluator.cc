#include "intrusion_evaluator.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
bool is_finite(float value)
{
    return std::isfinite(static_cast<double>(value));
}

long long non_negative_delta(long long current, long long start)
{
    return current >= start ? current - start : 0;
}
}

bool validate_intrusion_rule_config(const IntrusionRuleConfig &config, std::string *error_message)
{
    if (!config.enabled)
    {
        if (error_message != NULL) error_message->clear();
        return true;
    }
    if (config.rule_id.empty())
    {
        if (error_message != NULL) *error_message = "rule_id must not be empty";
        return false;
    }
    if (config.class_ids.empty())
    {
        if (error_message != NULL) *error_message = "class_ids must contain at least one class";
        return false;
    }
    std::set<int> class_ids;
    for (size_t i = 0; i < config.class_ids.size(); ++i)
    {
        if (config.class_ids[i] < 0 || !class_ids.insert(config.class_ids[i]).second)
        {
            if (error_message != NULL) *error_message = "class_ids must contain unique non-negative integers";
            return false;
        }
    }
    if (!config.region.enabled || !is_finite(config.region.x1) || !is_finite(config.region.y1) ||
        !is_finite(config.region.x2) || !is_finite(config.region.y2) || config.region.x1 < 0.0f ||
        config.region.y1 < 0.0f || config.region.x2 > 1.0f || config.region.y2 > 1.0f ||
        !(config.region.x1 < config.region.x2) || !(config.region.y1 < config.region.y2))
    {
        if (error_message != NULL) *error_message = "region must be an ordered normalized rectangle";
        return false;
    }
    if (config.dwell_ms <= 0)
    {
        if (error_message != NULL) *error_message = "dwell_ms must be positive";
        return false;
    }
    if (!is_finite(config.boundary_hysteresis_px) || config.boundary_hysteresis_px < 0.0f)
    {
        if (error_message != NULL) *error_message = "boundary_hysteresis_px must be non-negative";
        return false;
    }
    if (config.prediction_grace_ms < 0)
    {
        if (error_message != NULL) *error_message = "prediction_grace_ms must be non-negative";
        return false;
    }
    if (config.recent_event_capacity == 0)
    {
        if (error_message != NULL) *error_message = "recent_event_capacity must be positive";
        return false;
    }
    if (error_message != NULL) error_message->clear();
    return true;
}

const char *intrusion_target_state_to_string(IntrusionTargetState state)
{
    return state == IntrusionTargetState::Alarmed ? "alarmed" : "dwelling";
}

IntrusionEvaluator::IntrusionEvaluator(const IntrusionRuleConfig &config)
    : config_(config),
      valid_(false),
      validation_error_(),
      tracks_(),
      recent_events_(),
      event_sequence_(0),
      last_capture_timestamp_ms_(0)
{
    valid_ = validate_intrusion_rule_config(config_, &validation_error_);
}

bool IntrusionEvaluator::update(const TrackFrame &tracks, IntrusionFrame *output,
                                std::string *error_message)
{
    if (output == NULL)
    {
        if (error_message != NULL) *error_message = "intrusion output is null";
        return false;
    }
    *output = IntrusionFrame();
    output->enabled = config_.enabled;
    if (!config_.enabled)
    {
        output->state = "disabled";
        if (error_message != NULL) error_message->clear();
        return true;
    }
    if (!valid_)
    {
        return fail(validation_error_, output, error_message);
    }
    if (tracks.capture_frame_id <= 0 || tracks.capture_timestamp_ms <= 0 ||
        tracks.source_width <= 0 || tracks.source_height <= 0)
    {
        return fail("invalid track frame metadata", output, error_message);
    }
    if (last_capture_timestamp_ms_ > 0 && tracks.capture_timestamp_ms <= last_capture_timestamp_ms_)
    {
        return fail("track capture timestamp is not increasing", output, error_message);
    }

    std::map<uint64_t, RuntimeTrack> next_tracks = tracks_;
    std::vector<IntrusionEvent> new_events;
    std::set<uint64_t> seen_track_ids;

    for (size_t i = 0; i < tracks.objects.size(); ++i)
    {
        const TrackObject &track = tracks.objects[i];
        if (track.track_id == 0 || !seen_track_ids.insert(track.track_id).second)
        {
            return fail("track frame contains duplicate or zero track_id", output, error_message);
        }

        std::map<uint64_t, RuntimeTrack>::iterator existing = next_tracks.find(track.track_id);
        if (!class_allowed(track.class_id) || track.state != TrackLifecycle::Confirmed)
        {
            if (existing != next_tracks.end())
            {
                clear_runtime(track.track_id, tracks, &next_tracks, &new_events);
            }
            continue;
        }

        const bool observed = track.bbox_source == TrackerBBoxSource::Observed;
        RuntimeTrack &runtime = next_tracks[track.track_id];
        if (runtime.target.track_id == 0)
        {
            runtime.target.track_id = track.track_id;
            runtime.target.class_id = track.class_id;
            runtime.target.class_name = track.class_name;
        }

        if (!observed)
        {
            if (!runtime.dwelling_started || runtime.last_observed_timestamp_ms <= 0 ||
                non_negative_delta(tracks.capture_timestamp_ms, runtime.last_observed_timestamp_ms) >
                    config_.prediction_grace_ms)
            {
                if (runtime.alarmed)
                {
                    clear_runtime(track.track_id, tracks, &next_tracks, &new_events);
                }
                else
                {
                    next_tracks.erase(track.track_id);
                }
                continue;
            }
        }
        else
        {
            runtime.last_observed_timestamp_ms = tracks.capture_timestamp_ms;
        }

        const IntrusionTargetState state = runtime.alarmed ? IntrusionTargetState::Alarmed
                                                            : IntrusionTargetState::Dwelling;
        IntrusionTarget candidate = make_target(track, tracks, state,
                                                runtime.dwelling_started
                                                    ? non_negative_delta(tracks.capture_timestamp_ms,
                                                                          runtime.dwell_start_timestamp_ms)
                                                    : 0,
                                                runtime.target.event_sequence);
        const bool in_region = inside_region(candidate, tracks.source_width, tracks.source_height,
                                             runtime.dwelling_started);
        if (!runtime.dwelling_started)
        {
            if (!in_region)
            {
                next_tracks.erase(track.track_id);
                continue;
            }
            runtime.dwelling_started = true;
            runtime.dwell_start_timestamp_ms = tracks.capture_timestamp_ms;
            candidate.dwell_ms = 0;
        }
        else if (!in_region)
        {
            clear_runtime(track.track_id, tracks, &next_tracks, &new_events);
            continue;
        }

        runtime.last_seen_timestamp_ms = tracks.capture_timestamp_ms;
        const long long dwell_ms = non_negative_delta(tracks.capture_timestamp_ms,
                                                      runtime.dwell_start_timestamp_ms);
        if (observed && !runtime.alarmed && dwell_ms >= config_.dwell_ms)
        {
            runtime.alarmed = true;
            ++event_sequence_;
            runtime.target.event_sequence = event_sequence_;
            candidate.state = IntrusionTargetState::Alarmed;
            candidate.event_sequence = event_sequence_;
            append_event("alarmed", candidate, &new_events);
        }
        else
        {
            candidate.state = runtime.alarmed ? IntrusionTargetState::Alarmed
                                              : IntrusionTargetState::Dwelling;
            candidate.event_sequence = runtime.target.event_sequence;
        }
        candidate.dwell_ms = dwell_ms;
        runtime.target = candidate;
    }

    std::vector<uint64_t> expired_track_ids;
    for (std::map<uint64_t, RuntimeTrack>::const_iterator it = next_tracks.begin(); it != next_tracks.end(); ++it)
    {
        if (seen_track_ids.count(it->first) != 0)
        {
            continue;
        }
        const RuntimeTrack &runtime = it->second;
        if (!runtime.dwelling_started ||
            non_negative_delta(tracks.capture_timestamp_ms, runtime.last_seen_timestamp_ms) >
                config_.prediction_grace_ms)
        {
            expired_track_ids.push_back(it->first);
        }
    }
    for (size_t i = 0; i < expired_track_ids.size(); ++i)
        clear_runtime(expired_track_ids[i], tracks, &next_tracks, &new_events);

    for (std::map<uint64_t, RuntimeTrack>::iterator it = next_tracks.begin(); it != next_tracks.end(); ++it)
    {
        if (seen_track_ids.count(it->first) == 0 && it->second.dwelling_started)
        {
            it->second.target.dwell_ms = non_negative_delta(tracks.capture_timestamp_ms,
                                                            it->second.dwell_start_timestamp_ms);
            it->second.target.capture_frame_id = tracks.capture_frame_id;
            it->second.target.capture_timestamp_ms = tracks.capture_timestamp_ms;
        }
    }

    tracks_ = next_tracks;
    last_capture_timestamp_ms_ = tracks.capture_timestamp_ms;
    fill_output(tracks, tracks_, output);
    if (error_message != NULL) error_message->clear();
    return true;
}

void IntrusionEvaluator::reset()
{
    tracks_.clear();
    recent_events_.clear();
    event_sequence_ = 0;
    last_capture_timestamp_ms_ = 0;
}

const IntrusionRuleConfig &IntrusionEvaluator::config() const
{
    return config_;
}

uint64_t IntrusionEvaluator::event_sequence() const
{
    return event_sequence_;
}

bool IntrusionEvaluator::fail(const std::string &message, IntrusionFrame *output,
                              std::string *error_message) const
{
    output->enabled = config_.enabled;
    output->state = "unavailable";
    output->message = message;
    output->event_sequence = event_sequence_;
    if (error_message != NULL) *error_message = message;
    return false;
}

bool IntrusionEvaluator::class_allowed(int class_id) const
{
    return std::find(config_.class_ids.begin(), config_.class_ids.end(), class_id) !=
           config_.class_ids.end();
}

bool IntrusionEvaluator::inside_region(const IntrusionTarget &target, int source_width,
                                       int source_height, bool apply_hysteresis) const
{
    const float hysteresis = apply_hysteresis ? config_.boundary_hysteresis_px : 0.0f;
    const float left = config_.region.x1 * static_cast<float>(source_width) - hysteresis;
    const float top = config_.region.y1 * static_cast<float>(source_height) - hysteresis;
    const float right = config_.region.x2 * static_cast<float>(source_width) + hysteresis;
    const float bottom = config_.region.y2 * static_cast<float>(source_height) + hysteresis;
    return target.anchor_x >= left && target.anchor_x <= right && target.anchor_y >= top &&
           target.anchor_y <= bottom;
}

IntrusionTarget IntrusionEvaluator::make_target(const TrackObject &track, const TrackFrame &frame,
                                                IntrusionTargetState state, long long dwell_ms,
                                                uint64_t event_sequence) const
{
    IntrusionTarget target;
    target.track_id = track.track_id;
    target.class_id = track.class_id;
    target.class_name = track.class_name;
    target.bbox_source = track.bbox_source;
    target.bbox = track.bbox;
    target.anchor_x = (track.bbox.x1 + track.bbox.x2) * 0.5f;
    target.anchor_y = track.bbox.y2;
    target.state = state;
    target.dwell_ms = dwell_ms;
    target.threshold_ms = config_.dwell_ms;
    target.capture_frame_id = frame.capture_frame_id;
    target.capture_timestamp_ms = frame.capture_timestamp_ms;
    target.event_sequence = event_sequence;
    return target;
}

IntrusionTarget IntrusionEvaluator::make_target_from_runtime(const RuntimeTrack &runtime,
                                                             const TrackFrame &frame,
                                                             long long dwell_ms) const
{
    IntrusionTarget target = runtime.target;
    target.dwell_ms = dwell_ms;
    target.threshold_ms = config_.dwell_ms;
    target.capture_frame_id = frame.capture_frame_id;
    target.capture_timestamp_ms = frame.capture_timestamp_ms;
    target.state = runtime.alarmed ? IntrusionTargetState::Alarmed
                                   : IntrusionTargetState::Dwelling;
    return target;
}

void IntrusionEvaluator::append_event(const std::string &event_type, const IntrusionTarget &target,
                                      std::vector<IntrusionEvent> *events)
{
    IntrusionEvent event;
    event.event_sequence = target.event_sequence;
    event.event_type = event_type;
    event.target = target;
    events->push_back(event);
    recent_events_.push_back(event);
    while (recent_events_.size() > config_.recent_event_capacity)
        recent_events_.pop_front();
}

void IntrusionEvaluator::clear_runtime(uint64_t track_id, const TrackFrame &frame,
                                       std::map<uint64_t, RuntimeTrack> *next_tracks,
                                       std::vector<IntrusionEvent> *new_events)
{
    std::map<uint64_t, RuntimeTrack>::iterator it = next_tracks->find(track_id);
    if (it == next_tracks->end()) return;
    if (it->second.alarmed)
    {
        IntrusionTarget target = make_target_from_runtime(
            it->second, frame,
            non_negative_delta(frame.capture_timestamp_ms, it->second.dwell_start_timestamp_ms));
        ++event_sequence_;
        target.event_sequence = event_sequence_;
        append_event("cleared", target, new_events);
    }
    next_tracks->erase(it);
}

void IntrusionEvaluator::fill_output(const TrackFrame &frame,
                                     const std::map<uint64_t, RuntimeTrack> &tracks,
                                     IntrusionFrame *output) const
{
    output->enabled = true;
    output->state = "running";
    output->capture_frame_id = frame.capture_frame_id;
    output->capture_timestamp_ms = frame.capture_timestamp_ms;
    output->source_width = frame.source_width;
    output->source_height = frame.source_height;
    output->event_sequence = event_sequence_;
    output->recent_events.assign(recent_events_.begin(), recent_events_.end());
    for (std::map<uint64_t, RuntimeTrack>::const_iterator it = tracks.begin(); it != tracks.end(); ++it)
    {
        const RuntimeTrack &runtime = it->second;
        if (!runtime.dwelling_started) continue;
        IntrusionTarget target = make_target_from_runtime(
            runtime, frame,
            non_negative_delta(frame.capture_timestamp_ms, runtime.dwell_start_timestamp_ms));
        output->in_region_targets.push_back(target);
        if (runtime.alarmed)
            output->active_alarms.push_back(target);
    }
}
