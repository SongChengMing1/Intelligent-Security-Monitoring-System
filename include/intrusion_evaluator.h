#ifndef RKNN_DETECT_INTRUSION_EVALUATOR_H_
#define RKNN_DETECT_INTRUSION_EVALUATOR_H_

#include <stdint.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "object_tracker.h"

struct IntrusionRegion
{
    bool enabled;
    float x1;
    float y1;
    float x2;
    float y2;

    IntrusionRegion()
        : enabled(false), x1(0.0f), y1(0.0f), x2(0.5f), y2(1.0f)
    {
    }
};

struct IntrusionRuleConfig
{
    bool enabled;
    std::string rule_id;
    std::vector<int> class_ids;
    IntrusionRegion region;
    long long dwell_ms;
    float boundary_hysteresis_px;
    long long prediction_grace_ms;
    size_t recent_event_capacity;

    IntrusionRuleConfig()
        : enabled(false),
          rule_id("person-intrusion"),
          class_ids(),
          region(),
          dwell_ms(5000),
          boundary_hysteresis_px(5.0f),
          prediction_grace_ms(1000),
          recent_event_capacity(16)
    {
    }
};

bool validate_intrusion_rule_config(const IntrusionRuleConfig &config, std::string *error_message);

enum class IntrusionTargetState
{
    Dwelling,
    Alarmed,
};

const char *intrusion_target_state_to_string(IntrusionTargetState state);

struct IntrusionTarget
{
    uint64_t track_id;
    int class_id;
    std::string class_name;
    TrackerBBoxSource bbox_source;
    TrackerBBox bbox;
    float anchor_x;
    float anchor_y;
    IntrusionTargetState state;
    long long dwell_ms;
    long long threshold_ms;
    long long capture_frame_id;
    long long capture_timestamp_ms;
    uint64_t event_sequence;

    IntrusionTarget()
        : track_id(0),
          class_id(-1),
          class_name(),
          bbox_source(TrackerBBoxSource::Observed),
          bbox(),
          anchor_x(0.0f),
          anchor_y(0.0f),
          state(IntrusionTargetState::Dwelling),
          dwell_ms(0),
          threshold_ms(0),
          capture_frame_id(0),
          capture_timestamp_ms(0),
          event_sequence(0)
    {
    }
};

struct IntrusionEvent
{
    uint64_t event_sequence;
    std::string event_type;
    IntrusionTarget target;

    IntrusionEvent() : event_sequence(0), event_type(), target() {}
};

struct IntrusionFrame
{
    bool enabled;
    std::string state;
    long long capture_frame_id;
    long long capture_timestamp_ms;
    int source_width;
    int source_height;
    uint64_t event_sequence;
    std::vector<IntrusionTarget> in_region_targets;
    std::vector<IntrusionTarget> active_alarms;
    std::vector<IntrusionEvent> recent_events;
    std::string message;

    IntrusionFrame()
        : enabled(false),
          state("disabled"),
          capture_frame_id(0),
          capture_timestamp_ms(0),
          source_width(0),
          source_height(0),
          event_sequence(0),
          in_region_targets(),
          active_alarms(),
          recent_events(),
          message()
    {
    }
};

class IntrusionEvaluator
{
public:
    explicit IntrusionEvaluator(const IntrusionRuleConfig &config);

    bool update(const TrackFrame &tracks, IntrusionFrame *output, std::string *error_message);
    void reset();
    const IntrusionRuleConfig &config() const;
    uint64_t event_sequence() const;

private:
    struct RuntimeTrack
    {
        bool dwelling_started;
        bool alarmed;
        long long dwell_start_timestamp_ms;
        long long last_observed_timestamp_ms;
        long long last_seen_timestamp_ms;
        IntrusionTarget target;

        RuntimeTrack()
            : dwelling_started(false),
              alarmed(false),
              dwell_start_timestamp_ms(0),
              last_observed_timestamp_ms(0),
              last_seen_timestamp_ms(0),
              target()
        {
        }
    };

    bool fail(const std::string &message, IntrusionFrame *output, std::string *error_message) const;
    bool class_allowed(int class_id) const;
    bool inside_region(const IntrusionTarget &target, int source_width, int source_height,
                       bool apply_hysteresis) const;
    IntrusionTarget make_target(const TrackObject &track, const TrackFrame &frame,
                                IntrusionTargetState state, long long dwell_ms,
                                uint64_t event_sequence) const;
    IntrusionTarget make_target_from_runtime(const RuntimeTrack &runtime,
                                             const TrackFrame &frame,
                                             long long dwell_ms) const;
    void append_event(const std::string &event_type, const IntrusionTarget &target,
                      std::vector<IntrusionEvent> *events);
    void clear_runtime(uint64_t track_id, const TrackFrame &frame,
                       std::map<uint64_t, RuntimeTrack> *next_tracks,
                       std::vector<IntrusionEvent> *new_events);
    void fill_output(const TrackFrame &frame, const std::map<uint64_t, RuntimeTrack> &tracks,
                     IntrusionFrame *output) const;

    IntrusionRuleConfig config_;
    bool valid_;
    std::string validation_error_;
    std::map<uint64_t, RuntimeTrack> tracks_;
    std::deque<IntrusionEvent> recent_events_;
    uint64_t event_sequence_;
    long long last_capture_timestamp_ms_;
};

#endif  // RKNN_DETECT_INTRUSION_EVALUATOR_H_
