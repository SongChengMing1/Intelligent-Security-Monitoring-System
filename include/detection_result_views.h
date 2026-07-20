#ifndef RKNN_DETECT_DETECTION_RESULT_VIEWS_H_
#define RKNN_DETECT_DETECTION_RESULT_VIEWS_H_

#include "postprocess.h"

struct DetectionResultViews
{
    float low_threshold;
    float high_threshold;
    int candidate_count;
    int below_low_count;
    int high_pass_count;
    int low_pass_count;
    int low_only_count;
    int low_pass_high_duplicate_count;
    int rejected_low_pass_high_score_count;
    int suppressed_low_only_overlap_count;
    int capacity_dropped_count;
    detect_result_group_t tracker_candidates;
    detect_result_group_t display_detections;

    DetectionResultViews();
};

int build_detection_result_views(const detect_result_group_t *postprocess_results,
                                 float low_threshold,
                                 float high_threshold,
                                 DetectionResultViews *views);

int build_dual_pass_detection_result_views(const detect_result_group_t *high_pass_results,
                                           const detect_result_group_t *low_pass_results,
                                           float low_threshold,
                                           float high_threshold,
                                           float overlap_threshold,
                                           DetectionResultViews *views);

#endif
