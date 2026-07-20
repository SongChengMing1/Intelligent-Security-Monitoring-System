#ifndef RKNN_DETECT_TRACKING_PROFILE_H_
#define RKNN_DETECT_TRACKING_PROFILE_H_

#include <string>
#include <vector>

#include "object_tracker.h"

static const int kTrackingProfileSchemaVersion = 1;

struct TrackingRoiConfig
{
    bool enabled;
    float x1;
    float y1;
    float x2;
    float y2;

    TrackingRoiConfig() : enabled(false), x1(0.0f), y1(0.0f), x2(1.0f), y2(1.0f) {}
};

struct ObservationPolicyConfig
{
    std::vector<int> allowed_class_ids;
    float min_width;
    float min_height;
    float min_area;
    float edge_margin;
    TrackingRoiConfig roi;
    size_t max_observations;

    ObservationPolicyConfig()
        : allowed_class_ids(),
          min_width(0.0f),
          min_height(0.0f),
          min_area(0.0f),
          edge_margin(0.0f),
          roi(),
          max_observations(256)
    {
    }
};

struct ResolvedTrackingProfile
{
    int profile_schema_version;
    std::string profile_id;
    float detector_high_threshold;
    float detector_nms_threshold;
    ObservationPolicyConfig observation;
    std::string tracker_type;
    TrackerConfig tracker;

    ResolvedTrackingProfile()
        : profile_schema_version(kTrackingProfileSchemaVersion),
          profile_id("default-general"),
          detector_high_threshold(0.50f),
          detector_nms_threshold(0.60f),
          observation(),
          tracker_type("bytetrack"),
          tracker()
    {
        tracker.low_threshold = 0.35f;
        tracker.high_threshold = 0.50f;
        tracker.new_track_threshold = 0.50f;
        tracker.match_threshold = 0.80f;
        tracker.second_match_threshold = 0.50f;
        tracker.confirm_hits = 2;
        tracker.lost_timeout_ms = 1000;
        tracker.max_tracks = 64;
    }
};

bool resolve_tracking_profile_json(const std::string &source_json,
                                   ResolvedTrackingProfile *profile,
                                   std::string *error_message);
bool validate_resolved_tracking_profile(const ResolvedTrackingProfile &profile,
                                        std::string *error_message);
std::string canonical_tracking_profile_json(const ResolvedTrackingProfile &profile);
std::string hash_canonical_tracking_profile(const ResolvedTrackingProfile &profile);

#endif
