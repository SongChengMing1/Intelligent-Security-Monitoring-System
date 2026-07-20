#include "observation_policy.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
void reject(AnalysisObservation *observation,
            ObservationRejectionReason reason,
            std::map<std::string, size_t> *counts)
{
    observation->rejection_reasons.push_back(reason);
    ++(*counts)[observation_rejection_reason_to_string(reason)];
}
} // namespace

ObservationPolicy::ObservationPolicy(const ObservationPolicyConfig &config, float tracker_low_threshold)
    : config_(config), tracker_low_threshold_(tracker_low_threshold)
{
}

bool ObservationPolicy::apply(const std::vector<AnalysisObservation> &input,
                              int source_width,
                              int source_height,
                              ObservationPolicyResult *result,
                              std::string *error_message) const
{
    if (result == NULL)
    {
        if (error_message != NULL)
            *error_message = "observation policy result is null";
        return false;
    }
    if (source_width <= 0 || source_height <= 0)
    {
        if (error_message != NULL)
            *error_message = "source dimensions must be positive";
        return false;
    }

    result->observations = input;
    result->admitted_observation_ids.clear();
    result->rejected_by_reason.clear();
    const std::set<int> allowed(config_.allowed_class_ids.begin(), config_.allowed_class_ids.end());
    std::set<uint32_t> seen_ids;
    size_t admitted_count = 0;

    for (size_t i = 0; i < result->observations.size(); ++i)
    {
        AnalysisObservation &observation = result->observations[i];
        observation.admitted = false;
        observation.rejection_reasons.clear();
        if (observation.observation_id == 0 || !seen_ids.insert(observation.observation_id).second)
        {
            if (error_message != NULL)
                *error_message = "observation IDs must be non-zero and unique within a frame";
            return false;
        }

        const float width = observation.bbox.x2 - observation.bbox.x1;
        const float height = observation.bbox.y2 - observation.bbox.y1;
        const float center_x = (observation.bbox.x1 + observation.bbox.x2) * 0.5f;
        const float center_y = (observation.bbox.y1 + observation.bbox.y2) * 0.5f;
        const bool finite = std::isfinite(observation.score) && std::isfinite(observation.bbox.x1) &&
                            std::isfinite(observation.bbox.y1) && std::isfinite(observation.bbox.x2) &&
                            std::isfinite(observation.bbox.y2);
        if (!finite || observation.class_id < 0 || width <= 0.0f || height <= 0.0f)
            reject(&observation, ObservationRejectionReason::InvalidBBox, &result->rejected_by_reason);
        if (!allowed.empty() && allowed.count(observation.class_id) == 0)
            reject(&observation, ObservationRejectionReason::ClassNotAllowed, &result->rejected_by_reason);
        if (observation.score < tracker_low_threshold_)
            reject(&observation, ObservationRejectionReason::BelowTrackerLowThreshold, &result->rejected_by_reason);
        if (width < config_.min_width)
            reject(&observation, ObservationRejectionReason::BelowMinWidth, &result->rejected_by_reason);
        if (height < config_.min_height)
            reject(&observation, ObservationRejectionReason::BelowMinHeight, &result->rejected_by_reason);
        if (width * height < config_.min_area)
            reject(&observation, ObservationRejectionReason::BelowMinArea, &result->rejected_by_reason);
        if (center_x < config_.edge_margin || center_x >= source_width - config_.edge_margin ||
            center_y < config_.edge_margin || center_y >= source_height - config_.edge_margin)
            reject(&observation, ObservationRejectionReason::InsideEdgeMargin, &result->rejected_by_reason);
        if (config_.roi.enabled)
        {
            const float normalized_x = center_x / static_cast<float>(source_width);
            const float normalized_y = center_y / static_cast<float>(source_height);
            if (normalized_x < config_.roi.x1 || normalized_x >= config_.roi.x2 ||
                normalized_y < config_.roi.y1 || normalized_y >= config_.roi.y2)
                reject(&observation, ObservationRejectionReason::OutsideRoi, &result->rejected_by_reason);
        }
        if (observation.rejection_reasons.empty() && admitted_count >= config_.max_observations)
            reject(&observation, ObservationRejectionReason::CapacityLimit, &result->rejected_by_reason);
        if (observation.rejection_reasons.empty())
        {
            observation.admitted = true;
            result->admitted_observation_ids.push_back(observation.observation_id);
            ++admitted_count;
        }
    }
    if (error_message != NULL)
        error_message->clear();
    return true;
}
