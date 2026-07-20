#include "tracking_replay.h"

#include <cmath>
#include <map>
#include <stdexcept>

namespace
{
DetectionFrame tracker_frame(const AnalysisRecord &record, const std::vector<uint32_t> &ids,
                             const std::vector<AnalysisObservation> &observations)
{
    DetectionFrame frame; frame.capture_frame_id = record.capture_frame_id; frame.capture_timestamp_ms = record.capture_timestamp_ms;
    frame.source_width = record.source_width; frame.source_height = record.source_height;
    std::map<uint32_t, const AnalysisObservation *> by_id;
    for (size_t i = 0; i < observations.size(); ++i) by_id[observations[i].observation_id] = &observations[i];
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (!by_id.count(ids[i])) throw std::runtime_error("tracker input references missing observation ID");
        const AnalysisObservation &source = *by_id[ids[i]]; DetectionObject object; object.class_id = source.class_id;
        object.class_name = source.class_name; object.score = source.score; object.bbox = source.bbox; frame.objects.push_back(object);
    }
    return frame;
}
} // namespace

const char *replay_mode_to_string(ReplayMode mode)
{
    if (mode == ReplayMode::TrackerRerun) return "tracker-rerun";
    if (mode == ReplayMode::PolicyTrackerRerun) return "policy-tracker-rerun";
    return "recorded";
}

bool run_tracking_replay(const ReplaySource &source, ReplayMode mode, const ResolvedTrackingProfile &profile,
                         bool allow_fixed_detector_mismatch, ReplayRunResult *result, std::string *error_message)
{
    if (!result) return false; *result = ReplayRunResult();
    const bool detector_match = std::fabs(source.source_profile.detector_high_threshold - profile.detector_high_threshold) <= 1e-6 &&
                                std::fabs(source.source_profile.detector_nms_threshold - profile.detector_nms_threshold) <= 1e-6;
    result->detector_comparability = detector_match ? "exact" : "fixed_detector_mismatch";
    if (!detector_match && mode != ReplayMode::Recorded && !allow_fixed_detector_mismatch)
    { if (error_message) *error_message = "selected detector conditions differ; recorded post-NMS observations cannot be recomputed"; return false; }
    if (!detector_match) result->warnings.push_back("fixed detector mismatch: only recorded observations are used");
    std::unique_ptr<IObjectTracker> tracker;
    if (mode != ReplayMode::Recorded)
    {
        std::string tracker_error; tracker = create_bytetrack_object_tracker(profile.tracker, &tracker_error);
        if (!tracker) { if (error_message) *error_message = tracker_error; return false; } tracker->reset();
    }
    ObservationPolicy policy(profile.observation, profile.tracker.low_threshold);
    try
    {
        for (size_t i = 0; i < source.records.size(); ++i)
        {
            const AnalysisRecord &record = source.records[i]; ReplayFrameResult frame;
            frame.capture_frame_id = record.capture_frame_id; frame.capture_timestamp_ms = record.capture_timestamp_ms;
            if (mode == ReplayMode::Recorded) { frame.policy.observations = record.observations; frame.policy.admitted_observation_ids = record.tracker_input_observation_ids; frame.tracks = record.recorded_tracks; }
            else
            {
                if (mode == ReplayMode::TrackerRerun) { frame.policy.observations = record.observations; frame.policy.admitted_observation_ids = record.tracker_input_observation_ids; }
                else { std::string policy_error; if (!policy.apply(record.observations, record.source_width, record.source_height, &frame.policy, &policy_error)) throw std::runtime_error(policy_error); }
                const DetectionFrame input = tracker_frame(record, frame.policy.admitted_observation_ids, frame.policy.observations);
                std::string tracker_error; if (!tracker->update(input, &frame.tracks, &tracker_error)) throw std::runtime_error(tracker_error);
            }
            result->frames.push_back(frame);
        }
        if (error_message) error_message->clear(); return true;
    }
    catch (const std::exception &exception) { if (error_message) *error_message = exception.what(); return false; }
}
