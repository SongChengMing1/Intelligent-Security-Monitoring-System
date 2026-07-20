#ifndef RKNN_DETECT_TRACKING_REPLAY_H_
#define RKNN_DETECT_TRACKING_REPLAY_H_

#include <string>
#include <vector>

#include "analysis_record.h"
#include "observation_policy.h"
#include "tracking_profile.h"

enum class ReplayMode { Recorded, TrackerRerun, PolicyTrackerRerun };

struct ReplayFrameResult
{
    long long capture_frame_id;
    long long capture_timestamp_ms;
    ObservationPolicyResult policy;
    TrackFrame tracks;
};

struct ReplayRunResult
{
    std::vector<ReplayFrameResult> frames;
    std::string detector_comparability;
    std::vector<std::string> warnings;
};

struct ReplaySource
{
    std::string session_path;
    std::string session_id;
    std::string manifest_text;
    std::string manifest_hash;
    std::string source_git_commit;
    std::string source_profile_hash;
    ResolvedTrackingProfile source_profile;
    std::vector<AnalysisRecord> records;
};

bool load_replay_source(const std::string &session_path, ReplaySource *source, std::string *error_message);
bool run_tracking_replay(const ReplaySource &source, ReplayMode mode, const ResolvedTrackingProfile &profile,
                         bool allow_fixed_detector_mismatch, ReplayRunResult *result, std::string *error_message);
bool write_replay_output(const std::string &output_path, const ReplaySource &source, ReplayMode mode,
                         const ResolvedTrackingProfile &profile, const ReplayRunResult &result, std::string *error_message);

enum class ReplayComparison { Exact, WithinFloatTolerance, SemanticDifference, DetectorIncomparable };
ReplayComparison compare_replay_results(const ReplayRunResult &left, const ReplayRunResult &right,
                                        double float_tolerance, std::string *detail);
const char *replay_comparison_to_string(ReplayComparison comparison);
const char *replay_mode_to_string(ReplayMode mode);

#endif
