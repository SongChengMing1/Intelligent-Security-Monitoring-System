#include <iostream>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <string>

#include "tracking_replay.h"

namespace { bool expect(bool value, const std::string &message) { if (!value) std::cerr << "FAIL: " << message << std::endl; return value; } }

int main(int argc, char **argv)
{
    if (argc != 2) return 2; bool ok = true; ReplaySource source; source.session_id = "fixture"; source.source_profile = ResolvedTrackingProfile();
    source.source_profile.profile_id = "default-general"; source.source_profile.detector_high_threshold = 0.5f; source.source_profile.detector_nms_threshold = 0.6f;
    AnalysisRecord first; first.capture_frame_id = 1; first.capture_timestamp_ms = 1000; first.source_width = 640; first.source_height = 480;
    AnalysisObservation observation; observation.observation_id = 1; observation.class_id = 0; observation.class_name = "person"; observation.score = 0.8f;
    observation.bbox.x1 = 10; observation.bbox.y1 = 10; observation.bbox.x2 = 50; observation.bbox.y2 = 80; observation.admitted = true;
    first.observations.push_back(observation); first.tracker_input_observation_ids.push_back(1); first.recorded_tracks.capture_frame_id = 1; first.recorded_tracks.capture_timestamp_ms = 1000;
    AnalysisRecord empty = first; empty.capture_frame_id = 2; empty.capture_timestamp_ms = 1100; empty.observations.clear(); empty.tracker_input_observation_ids.clear();
    source.records.push_back(first); source.records.push_back(empty);
    ReplayRunResult recorded, tracker, policy, repeated; std::string error;
    ok &= expect(run_tracking_replay(source, ReplayMode::Recorded, source.source_profile, false, &recorded, &error), "recorded mode");
    ok &= expect(run_tracking_replay(source, ReplayMode::TrackerRerun, source.source_profile, false, &tracker, &error), "tracker rerun");
    ok &= expect(run_tracking_replay(source, ReplayMode::PolicyTrackerRerun, source.source_profile, false, &policy, &error), "policy rerun");
    ok &= expect(run_tracking_replay(source, ReplayMode::PolicyTrackerRerun, source.source_profile, false, &repeated, &error), "repeated rerun");
    std::string detail; ok &= expect(compare_replay_results(policy, repeated, 1e-6, &detail) == ReplayComparison::Exact, "deterministic repeated rerun");
    ResolvedTrackingProfile mismatch = source.source_profile; mismatch.detector_nms_threshold = 0.4f; ReplayRunResult mismatch_result;
    ok &= expect(!run_tracking_replay(source, ReplayMode::TrackerRerun, mismatch, false, &mismatch_result, &error), "detector mismatch rejected");
    ok &= expect(run_tracking_replay(source, ReplayMode::TrackerRerun, mismatch, true, &mismatch_result, &error), "detector mismatch override");
    ok &= expect(compare_replay_results(mismatch_result, tracker, 1e-6, &detail) == ReplayComparison::DetectorIncomparable, "mismatch classified incomparable");
    ResolvedTrackingProfile policy_difference = source.source_profile; policy_difference.observation.allowed_class_ids.clear(); policy_difference.observation.allowed_class_ids.push_back(2);
    ReplayRunResult filtered; ok &= expect(run_tracking_replay(source, ReplayMode::PolicyTrackerRerun, policy_difference, false, &filtered, &error), "different observation Profile reruns");
    ok &= expect(filtered.frames[0].policy.admitted_observation_ids.empty(), "different Profile changes policy without RKNN");
    ReplayRunResult tolerance = tracker; if (!tolerance.frames.empty() && !tolerance.frames[0].tracks.objects.empty()) tolerance.frames[0].tracks.objects[0].bbox.x1 += 0.00001f;
    ok &= expect(compare_replay_results(tracker, tolerance, 0.001, &detail) == ReplayComparison::WithinFloatTolerance, "float tolerance classification");
    ReplaySource loaded; const std::string session_path = std::string(argv[1]) + "/v351_session";
    ok &= expect(load_replay_source(session_path, &loaded, &error), "golden session loads: " + error);
    ReplayRunResult loaded_run; ok &= expect(run_tracking_replay(loaded, ReplayMode::TrackerRerun, loaded.source_profile, false, &loaded_run, &error), "loaded session reruns");
    std::ostringstream output; output << "/tmp/rknn_detect_replay_output_" << getpid();
    ok &= expect(write_replay_output(output.str(), loaded, ReplayMode::TrackerRerun, loaded.source_profile, loaded_run, &error), "replay output writes: " + error);
    std::ifstream output_manifest((output.str() + "/manifest.json").c_str()); std::ifstream output_tracks((output.str() + "/tracks.jsonl").c_str());
    ok &= expect(output_manifest.good() && output_tracks.good(), "replay manifest and tracks exist");
    nlohmann::json replay_manifest; output_manifest >> replay_manifest;
    ok &= expect(replay_manifest.count("source_git_commit") && replay_manifest.count("replay_git_commit") && replay_manifest.at("mode") == "tracker-rerun", "replay provenance is complete");
    return ok ? 0 : 1;
}
