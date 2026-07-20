#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "observation_policy.h"

namespace
{
AnalysisObservation make_observation(uint32_t id, int class_id, float score,
                                     float x1, float y1, float x2, float y2)
{
    AnalysisObservation value;
    value.observation_id = id;
    value.class_id = class_id;
    value.class_name = "fixture";
    value.score = score;
    value.bbox.x1 = x1;
    value.bbox.y1 = y1;
    value.bbox.x2 = x2;
    value.bbox.y2 = y2;
    return value;
}

bool expect(bool condition, const std::string &message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << std::endl;
    return condition;
}
} // namespace

int main()
{
    bool ok = true;
    ObservationPolicyConfig config;
    config.allowed_class_ids.push_back(0);
    config.min_width = 20.0f;
    config.min_height = 30.0f;
    config.min_area = 800.0f;
    config.edge_margin = 10.0f;
    config.roi.enabled = true;
    config.roi.x1 = 0.1f;
    config.roi.y1 = 0.1f;
    config.roi.x2 = 0.9f;
    config.roi.y2 = 0.9f;
    config.max_observations = 2;
    ObservationPolicy policy(config, 0.35f);

    std::vector<AnalysisObservation> input;
    input.push_back(make_observation(1, 0, 0.8f, 100, 100, 140, 150));
    input.push_back(make_observation(2, 2, 0.2f, 1, 1, 5, 6));
    input.push_back(make_observation(3, 0, 0.7f, 200, 100, 240, 150));
    input.push_back(make_observation(4, 0, 0.7f, 300, 100, 340, 150));
    ObservationPolicyResult result;
    std::string error;
    ok &= expect(policy.apply(input, 640, 480, &result, &error), "policy applies");
    ok &= expect(result.admitted_observation_ids.size() == 2, "capacity is bounded");
    ok &= expect(result.admitted_observation_ids[0] == 1 && result.admitted_observation_ids[1] == 3,
                 "admission preserves input order");
    ok &= expect(result.observations[1].rejection_reasons.size() >= 6,
                 "all applicable rejection reasons are retained");
    ok &= expect(result.rejected_by_reason["class_not_allowed"] == 1, "class count");
    ok &= expect(result.rejected_by_reason["capacity_limit"] == 1, "capacity count");

    std::vector<AnalysisObservation> empty;
    ok &= expect(policy.apply(empty, 640, 480, &result, &error) && result.observations.empty(),
                 "empty frame is valid");

    std::vector<AnalysisObservation> invalid;
    invalid.push_back(make_observation(1, 0, 0.8f, 10, 10,
                                       std::numeric_limits<float>::quiet_NaN(), 20));
    ok &= expect(policy.apply(invalid, 640, 480, &result, &error), "invalid bbox is a decision, not frame failure");
    ok &= expect(result.rejected_by_reason["invalid_bbox"] == 1, "invalid bbox reason");

    std::vector<AnalysisObservation> duplicate;
    duplicate.push_back(make_observation(1, 0, 0.8f, 100, 100, 140, 150));
    duplicate.push_back(make_observation(1, 0, 0.8f, 200, 100, 240, 150));
    ok &= expect(!policy.apply(duplicate, 640, 480, &result, &error), "duplicate IDs reject the frame contract");

    return ok ? 0 : 1;
}
