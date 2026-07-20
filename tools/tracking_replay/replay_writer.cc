#include "tracking_replay.h"

#include <errno.h>
#include <sys/stat.h>

#include <fstream>

#include <nlohmann/json.hpp>

#ifndef RKNN_DETECT_GIT_COMMIT
#define RKNN_DETECT_GIT_COMMIT "unknown"
#endif

namespace
{
using nlohmann::json;
json track_json(const TrackFrame &frame)
{
    json objects = json::array();
    for (size_t i = 0; i < frame.objects.size(); ++i)
    {
        const TrackObject &track = frame.objects[i];
        objects.push_back({{"track_id", track.track_id}, {"class_id", track.class_id}, {"class_name", track.class_name},
                           {"state", track_lifecycle_to_string(track.state)}, {"bbox_source", tracker_bbox_source_to_string(track.bbox_source)},
                           {"score", track.score}, {"bbox", {track.bbox.x1, track.bbox.y1, track.bbox.x2, track.bbox.y2}},
                           {"track_age", track.track_age}, {"hit_count", track.hit_count}, {"missed_updates", track.missed_updates}});
    }
    return {{"capture_frame_id", frame.capture_frame_id}, {"capture_timestamp_ms", frame.capture_timestamp_ms},
            {"source_width", frame.source_width}, {"source_height", frame.source_height}, {"objects", objects}};
}
}

bool write_replay_output(const std::string &output_path, const ReplaySource &source, ReplayMode mode,
                         const ResolvedTrackingProfile &profile, const ReplayRunResult &result, std::string *error_message)
{
    if (mkdir(output_path.c_str(), 0755) != 0 && errno != EEXIST) { if (error_message) *error_message = "cannot create replay output"; return false; }
    try
    {
        json manifest = {{"replay_manifest_schema_version", kReplayManifestSchemaVersion}, {"source_session_id", source.session_id},
                         {"source_session_hash", source.manifest_hash}, {"source_git_commit", source.source_git_commit},
                         {"replay_git_commit", RKNN_DETECT_GIT_COMMIT}, {"mode", replay_mode_to_string(mode)},
                         {"policy_source", mode == ReplayMode::TrackerRerun ? "recorded" : mode == ReplayMode::PolicyTrackerRerun ? "rerun" : "recorded"},
                         {"detector_comparability", result.detector_comparability}, {"effective_profile_id", profile.profile_id},
                         {"effective_profile_hash", hash_canonical_tracking_profile(profile)},
                         {"resolved_profile", nlohmann::json::parse(canonical_tracking_profile_json(profile))},
                         {"tracker_implementation", "production-bytetrack"}, {"warnings", result.warnings}};
        std::ofstream manifest_file((output_path + "/manifest.json").c_str()); manifest_file << manifest.dump(2) << "\n";
        std::ofstream tracks((output_path + "/tracks.jsonl").c_str());
        std::ofstream decisions;
        if (mode == ReplayMode::PolicyTrackerRerun) decisions.open((output_path + "/policy-decisions.jsonl").c_str());
        for (size_t i = 0; i < result.frames.size(); ++i)
        {
            tracks << track_json(result.frames[i].tracks).dump() << "\n";
            if (decisions.is_open()) decisions << json({{"capture_frame_id", result.frames[i].capture_frame_id},
                {"admitted_observation_ids", result.frames[i].policy.admitted_observation_ids}}).dump() << "\n";
        }
        json status = {{"replay_status_schema_version", kReplayStatusSchemaVersion}, {"state", "completed"},
                       {"processed_records", result.frames.size()}, {"warnings", result.warnings}};
        std::ofstream status_file((output_path + "/replay-status.json").c_str()); status_file << status.dump(2) << "\n";
        if (!manifest_file || !tracks || !status_file || (decisions.is_open() && !decisions)) throw std::runtime_error("replay output write failed");
        if (error_message) error_message->clear(); return true;
    }
    catch (const std::exception &exception) { if (error_message) *error_message = exception.what(); return false; }
}
