#include "tracking_profile.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sha256.h"

namespace
{
using nlohmann::json;

bool fail(const std::string &message, std::string *error_message)
{
    if (error_message != NULL)
    {
        *error_message = message;
    }
    return false;
}

template <typename T>
void assign_if_present(const json &object, const char *key, T *value)
{
    if (object.count(key) != 0)
    {
        *value = object.at(key).get<T>();
    }
}

double canonical_number(double value)
{
    return std::round(value * 1000000.0) / 1000000.0;
}

json to_algorithm_json(const ResolvedTrackingProfile &profile)
{
    json value;
    value["detector"] = {
        {"high_threshold", canonical_number(profile.detector_high_threshold)},
        {"nms_threshold", canonical_number(profile.detector_nms_threshold)}};
    value["observation"] = {
        {"allowed_class_ids", profile.observation.allowed_class_ids},
        {"edge_margin", canonical_number(profile.observation.edge_margin)},
        {"max_observations", profile.observation.max_observations},
        {"min_area", canonical_number(profile.observation.min_area)},
        {"min_height", canonical_number(profile.observation.min_height)},
        {"min_width", canonical_number(profile.observation.min_width)},
        {"roi", {
                    {"enabled", profile.observation.roi.enabled},
                    {"x1", canonical_number(profile.observation.roi.x1)},
                    {"x2", canonical_number(profile.observation.roi.x2)},
                    {"y1", canonical_number(profile.observation.roi.y1)},
                    {"y2", canonical_number(profile.observation.roi.y2)}}}};
    value["profile_id"] = profile.profile_id;
    value["profile_schema_version"] = profile.profile_schema_version;
    value["tracker"] = {
        {"confirm_hits", profile.tracker.confirm_hits},
        {"high_threshold", canonical_number(profile.tracker.high_threshold)},
        {"lost_timeout_ms", profile.tracker.lost_timeout_ms},
        {"match_threshold", canonical_number(profile.tracker.match_threshold)},
        {"max_tracks", profile.tracker.max_tracks},
        {"new_track_threshold", canonical_number(profile.tracker.new_track_threshold)},
        {"low_threshold", canonical_number(profile.tracker.low_threshold)},
        {"second_match_threshold", canonical_number(profile.tracker.second_match_threshold)},
        {"type", profile.tracker_type}};
    return value;
}
} // namespace

bool validate_resolved_tracking_profile(const ResolvedTrackingProfile &profile,
                                        std::string *error_message)
{
    if (profile.profile_schema_version != kTrackingProfileSchemaVersion)
        return fail("profile_schema_version must be 1", error_message);
    if (profile.profile_id.empty())
        return fail("profile_id must not be empty", error_message);
    if (!(profile.detector_high_threshold >= 0.0f && profile.detector_high_threshold <= 1.0f))
        return fail("detector.high_threshold must be in [0,1]", error_message);
    if (!(profile.detector_nms_threshold > 0.0f && profile.detector_nms_threshold <= 1.0f))
        return fail("detector.nms_threshold must be in (0,1]", error_message);
    if (profile.tracker_type != "bytetrack")
        return fail("tracker.type must be bytetrack", error_message);
    if (profile.tracker.low_threshold < 0.0f || profile.tracker.low_threshold > profile.tracker.high_threshold)
        return fail("tracker.low_threshold must be in [0, high_threshold]", error_message);
    if (profile.tracker.high_threshold < 0.0f || profile.tracker.high_threshold > 1.0f)
        return fail("tracker.high_threshold must be in [0,1]", error_message);
    if (profile.tracker.new_track_threshold < profile.tracker.high_threshold || profile.tracker.new_track_threshold > 1.0f)
        return fail("tracker.new_track_threshold must be in [high_threshold,1]", error_message);
    if (!(profile.tracker.match_threshold > 0.0f && profile.tracker.match_threshold <= 1.0f))
        return fail("tracker.match_threshold must be in (0,1]", error_message);
    if (!(profile.tracker.second_match_threshold > 0.0f && profile.tracker.second_match_threshold <= 1.0f))
        return fail("tracker.second_match_threshold must be in (0,1]", error_message);
    if (profile.tracker.confirm_hits < 1)
        return fail("tracker.confirm_hits must be at least 1", error_message);
    if (profile.tracker.lost_timeout_ms <= 0)
        return fail("tracker.lost_timeout_ms must be positive", error_message);
    if (profile.tracker.max_tracks == 0 || profile.tracker.max_tracks > 4096)
        return fail("tracker.max_tracks must be in [1,4096]", error_message);
    if (profile.observation.min_width < 0.0f || profile.observation.min_height < 0.0f ||
        profile.observation.min_area < 0.0f || profile.observation.edge_margin < 0.0f)
        return fail("observation size and edge values must be non-negative", error_message);
    if (profile.observation.max_observations == 0 || profile.observation.max_observations > 4096)
        return fail("observation.max_observations must be in [1,4096]", error_message);
    std::set<int> class_ids;
    for (size_t i = 0; i < profile.observation.allowed_class_ids.size(); ++i)
    {
        const int class_id = profile.observation.allowed_class_ids[i];
        if (class_id < 0)
            return fail("observation.allowed_class_ids must be non-negative", error_message);
        if (!class_ids.insert(class_id).second)
            return fail("observation.allowed_class_ids must be unique", error_message);
    }
    const TrackingRoiConfig &roi = profile.observation.roi;
    if (!(roi.x1 >= 0.0f && roi.x1 < roi.x2 && roi.x2 <= 1.0f &&
          roi.y1 >= 0.0f && roi.y1 < roi.y2 && roi.y2 <= 1.0f))
        return fail("observation.roi must satisfy 0 <= x1 < x2 <= 1 and 0 <= y1 < y2 <= 1", error_message);
    if (error_message != NULL)
        error_message->clear();
    return true;
}

bool resolve_tracking_profile_json(const std::string &source_json,
                                   ResolvedTrackingProfile *profile,
                                   std::string *error_message)
{
    if (profile == NULL)
        return fail("resolved profile output is null", error_message);
    try
    {
        const json source = json::parse(source_json);
        if (!source.is_object())
            return fail("profile root must be an object", error_message);
        ResolvedTrackingProfile result;
        assign_if_present(source, "profile_schema_version", &result.profile_schema_version);
        assign_if_present(source, "profile_id", &result.profile_id);
        if (source.count("detector"))
        {
            const json &detector = source.at("detector");
            assign_if_present(detector, "high_threshold", &result.detector_high_threshold);
            assign_if_present(detector, "nms_threshold", &result.detector_nms_threshold);
        }
        if (source.count("observation"))
        {
            const json &observation = source.at("observation");
            assign_if_present(observation, "allowed_class_ids", &result.observation.allowed_class_ids);
            assign_if_present(observation, "min_width", &result.observation.min_width);
            assign_if_present(observation, "min_height", &result.observation.min_height);
            assign_if_present(observation, "min_area", &result.observation.min_area);
            assign_if_present(observation, "edge_margin", &result.observation.edge_margin);
            assign_if_present(observation, "max_observations", &result.observation.max_observations);
            if (observation.count("roi"))
            {
                const json &roi = observation.at("roi");
                assign_if_present(roi, "enabled", &result.observation.roi.enabled);
                assign_if_present(roi, "x1", &result.observation.roi.x1);
                assign_if_present(roi, "y1", &result.observation.roi.y1);
                assign_if_present(roi, "x2", &result.observation.roi.x2);
                assign_if_present(roi, "y2", &result.observation.roi.y2);
            }
        }
        if (source.count("tracker"))
        {
            const json &tracker = source.at("tracker");
            assign_if_present(tracker, "type", &result.tracker_type);
            assign_if_present(tracker, "low_threshold", &result.tracker.low_threshold);
            assign_if_present(tracker, "high_threshold", &result.tracker.high_threshold);
            assign_if_present(tracker, "new_track_threshold", &result.tracker.new_track_threshold);
            assign_if_present(tracker, "match_threshold", &result.tracker.match_threshold);
            assign_if_present(tracker, "second_match_threshold", &result.tracker.second_match_threshold);
            assign_if_present(tracker, "confirm_hits", &result.tracker.confirm_hits);
            assign_if_present(tracker, "lost_timeout_ms", &result.tracker.lost_timeout_ms);
            assign_if_present(tracker, "max_tracks", &result.tracker.max_tracks);
        }
        if (!validate_resolved_tracking_profile(result, error_message))
            return false;
        *profile = result;
        return true;
    }
    catch (const std::exception &exception)
    {
        return fail(std::string("invalid profile JSON: ") + exception.what(), error_message);
    }
}

std::string canonical_tracking_profile_json(const ResolvedTrackingProfile &profile)
{
    return to_algorithm_json(profile).dump();
}

std::string hash_canonical_tracking_profile(const ResolvedTrackingProfile &profile)
{
    return std::string("sha256:") + sha256_hex(canonical_tracking_profile_json(profile));
}
