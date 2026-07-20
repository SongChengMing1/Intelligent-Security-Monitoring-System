#ifndef RKNN_DETECT_OBJECT_TRACKER_H_
#define RKNN_DETECT_OBJECT_TRACKER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

struct TrackerBBox
{
    float x1;
    float y1;
    float x2;
    float y2;

    TrackerBBox() : x1(0.0f), y1(0.0f), x2(0.0f), y2(0.0f) {}
};

struct DetectionObject
{
    int class_id;
    std::string class_name;
    float score;
    TrackerBBox bbox;

    DetectionObject() : class_id(-1), class_name(), score(0.0f), bbox() {}
};

struct DetectionFrame
{
    long long capture_frame_id;
    long long capture_timestamp_ms;
    int source_width;
    int source_height;
    std::vector<DetectionObject> objects;

    DetectionFrame()
        : capture_frame_id(0), capture_timestamp_ms(0), source_width(0), source_height(0), objects()
    {
    }
};

enum class TrackLifecycle
{
    Tentative,
    Confirmed,
    Lost,
    Removed,
};

enum class TrackerBBoxSource
{
    Observed,
    Predicted,
};

const char *track_lifecycle_to_string(TrackLifecycle state);
const char *tracker_bbox_source_to_string(TrackerBBoxSource source);

struct TrackObject
{
    uint64_t track_id;
    int class_id;
    std::string class_name;
    TrackLifecycle state;
    TrackerBBoxSource bbox_source;
    float score;
    TrackerBBox bbox;
    int track_age;
    int hit_count;
    int missed_updates;

    TrackObject()
        : track_id(0),
          class_id(-1),
          class_name(),
          state(TrackLifecycle::Tentative),
          bbox_source(TrackerBBoxSource::Observed),
          score(0.0f),
          bbox(),
          track_age(0),
          hit_count(0),
          missed_updates(0)
    {
    }
};

struct TrackFrame
{
    long long capture_frame_id;
    long long capture_timestamp_ms;
    int source_width;
    int source_height;
    std::vector<TrackObject> objects;

    TrackFrame()
        : capture_frame_id(0), capture_timestamp_ms(0), source_width(0), source_height(0), objects()
    {
    }
};

struct TrackerConfig
{
    float low_threshold;
    float high_threshold;
    float new_track_threshold;
    float match_threshold;
    float second_match_threshold;
    int confirm_hits;
    int lost_timeout_ms;
    int nominal_interval_ms;
    int max_timestamp_gap_ms;
    size_t max_tracks;

    TrackerConfig()
        : low_threshold(0.25f),
          high_threshold(0.50f),
          new_track_threshold(0.50f),
          match_threshold(0.80f),
          second_match_threshold(0.50f),
          confirm_hits(2),
          lost_timeout_ms(1000),
          nominal_interval_ms(100),
          max_timestamp_gap_ms(2000),
          max_tracks(64)
    {
    }
};

bool validate_tracker_config(const TrackerConfig &config, std::string *error_message);

struct TrackerDiagnostics
{
    long long update_count;
    long long reset_count;
    long long error_count;
    long long dropped_observations;
    size_t tentative_count;
    size_t confirmed_count;
    size_t lost_count;
    double last_update_ms;
    double average_update_ms;
    double max_update_ms;
    std::string last_error;

    TrackerDiagnostics()
        : update_count(0),
          reset_count(0),
          error_count(0),
          dropped_observations(0),
          tentative_count(0),
          confirmed_count(0),
          lost_count(0),
          last_update_ms(0.0),
          average_update_ms(0.0),
          max_update_ms(0.0),
          last_error()
    {
    }
};

class IObjectTracker
{
public:
    virtual ~IObjectTracker() {}
    virtual bool update(const DetectionFrame &detections, TrackFrame *tracks, std::string *error_message) = 0;
    virtual void reset() = 0;
    virtual TrackerDiagnostics diagnostics() const = 0;
};

std::unique_ptr<IObjectTracker> create_bytetrack_object_tracker(const TrackerConfig &config,
                                                                std::string *error_message);

#endif
