#ifndef RKNN_DETECT_OBSERVATION_POLICY_H_
#define RKNN_DETECT_OBSERVATION_POLICY_H_

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "analysis_record.h"
#include "tracking_profile.h"

struct ObservationPolicyResult
{
    std::vector<AnalysisObservation> observations;
    std::vector<uint32_t> admitted_observation_ids;
    std::map<std::string, size_t> rejected_by_reason;
};

class ObservationPolicy
{
public:
    explicit ObservationPolicy(const ObservationPolicyConfig &config, float tracker_low_threshold);

    bool apply(const std::vector<AnalysisObservation> &input,
               int source_width,
               int source_height,
               ObservationPolicyResult *result,
               std::string *error_message) const;

private:
    ObservationPolicyConfig config_;
    float tracker_low_threshold_;
};

#endif
