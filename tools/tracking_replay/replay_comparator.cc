#include "tracking_replay.h"

#include <cmath>

const char *replay_comparison_to_string(ReplayComparison value)
{
    if (value == ReplayComparison::WithinFloatTolerance) return "within_float_tolerance";
    if (value == ReplayComparison::SemanticDifference) return "semantic_difference";
    if (value == ReplayComparison::DetectorIncomparable) return "detector_incomparable";
    return "exact";
}

ReplayComparison compare_replay_results(const ReplayRunResult &left, const ReplayRunResult &right,
                                        double tolerance, std::string *detail)
{
    if (left.detector_comparability != "exact" || right.detector_comparability != "exact")
    { if (detail) *detail = "fixed detector mismatch"; return ReplayComparison::DetectorIncomparable; }
    if (left.frames.size() != right.frames.size()) { if (detail) *detail = "frame count differs"; return ReplayComparison::SemanticDifference; }
    bool tolerance_used = false;
    for (size_t i = 0; i < left.frames.size(); ++i)
    {
        const ReplayFrameResult &a = left.frames[i], &b = right.frames[i];
        if (a.capture_frame_id != b.capture_frame_id || a.policy.admitted_observation_ids != b.policy.admitted_observation_ids || a.tracks.objects.size() != b.tracks.objects.size())
        { if (detail) *detail = "frame identity, policy decision, or track count differs"; return ReplayComparison::SemanticDifference; }
        for (size_t j = 0; j < a.tracks.objects.size(); ++j)
        {
            const TrackObject &x = a.tracks.objects[j], &y = b.tracks.objects[j];
            if (x.track_id != y.track_id || x.class_id != y.class_id || x.state != y.state || x.bbox_source != y.bbox_source)
            { if (detail) *detail = "track semantic field differs"; return ReplayComparison::SemanticDifference; }
            const double differences[] = {std::fabs(x.score-y.score), std::fabs(x.bbox.x1-y.bbox.x1), std::fabs(x.bbox.y1-y.bbox.y1), std::fabs(x.bbox.x2-y.bbox.x2), std::fabs(x.bbox.y2-y.bbox.y2)};
            for (size_t k = 0; k < 5; ++k) { if (differences[k] > tolerance) { if (detail) *detail = "numeric field exceeds tolerance"; return ReplayComparison::SemanticDifference; } if (differences[k] > 0) tolerance_used = true; }
        }
    }
    if (detail) detail->clear(); return tolerance_used ? ReplayComparison::WithinFloatTolerance : ReplayComparison::Exact;
}
