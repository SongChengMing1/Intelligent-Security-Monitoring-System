#include "detection_result_views.h"

#include <algorithm>
#include <string.h>

DetectionResultViews::DetectionResultViews()
    : low_threshold(0.0f),
      high_threshold(0.0f),
      candidate_count(0),
      below_low_count(0),
      high_pass_count(0),
      low_pass_count(0),
      low_only_count(0),
      low_pass_high_duplicate_count(0),
      rejected_low_pass_high_score_count(0),
      suppressed_low_only_overlap_count(0),
      capacity_dropped_count(0),
      tracker_candidates(),
      display_detections()
{
    memset(&tracker_candidates, 0, sizeof(tracker_candidates));
    memset(&display_detections, 0, sizeof(display_detections));
}

static float result_iou(const detect_result_t &left, const detect_result_t &right)
{
    const int x1 = std::max(left.box.left, right.box.left);
    const int y1 = std::max(left.box.top, right.box.top);
    const int x2 = std::min(left.box.right, right.box.right);
    const int y2 = std::min(left.box.bottom, right.box.bottom);
    const float intersection = static_cast<float>(std::max(0, x2 - x1) * std::max(0, y2 - y1));
    const float left_area = static_cast<float>(std::max(0, left.box.right - left.box.left) *
                                               std::max(0, left.box.bottom - left.box.top));
    const float right_area = static_cast<float>(std::max(0, right.box.right - right.box.left) *
                                                std::max(0, right.box.bottom - right.box.top));
    const float union_area = left_area + right_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static bool overlaps_high_pass(const detect_result_t &result,
                               const detect_result_group_t &high_pass_results,
                               float overlap_threshold)
{
    for (int i = 0; i < high_pass_results.count; ++i)
    {
        if (result.class_id == high_pass_results.results[i].class_id &&
            result_iou(result, high_pass_results.results[i]) > overlap_threshold)
        {
            return true;
        }
    }
    return false;
}

int build_detection_result_views(const detect_result_group_t *postprocess_results,
                                 float low_threshold,
                                 float high_threshold,
                                 DetectionResultViews *views)
{
    if (postprocess_results == NULL || views == NULL ||
        low_threshold < 0.0f || low_threshold >= high_threshold || high_threshold > 1.0f ||
        postprocess_results->count < 0 || postprocess_results->count > OBJ_NUMB_MAX_SIZE)
    {
        return -1;
    }

    *views = DetectionResultViews();
    views->low_threshold = low_threshold;
    views->high_threshold = high_threshold;
    views->candidate_count = postprocess_results->count;
    views->tracker_candidates.id = postprocess_results->id;
    views->display_detections.id = postprocess_results->id;

    for (int i = 0; i < postprocess_results->count; ++i)
    {
        const detect_result_t &result = postprocess_results->results[i];
        if (result.prop < low_threshold)
        {
            views->below_low_count++;
            continue;
        }

        views->tracker_candidates.results[views->tracker_candidates.count++] = result;
        if (result.prop >= high_threshold)
        {
            views->display_detections.results[views->display_detections.count++] = result;
        }
    }
    return 0;
}

int build_dual_pass_detection_result_views(const detect_result_group_t *high_pass_results,
                                           const detect_result_group_t *low_pass_results,
                                           float low_threshold,
                                           float high_threshold,
                                           float overlap_threshold,
                                           DetectionResultViews *views)
{
    if (high_pass_results == NULL || low_pass_results == NULL || views == NULL ||
        low_threshold < 0.0f || low_threshold >= high_threshold || high_threshold > 1.0f ||
        overlap_threshold < 0.0f || overlap_threshold > 1.0f ||
        high_pass_results->count < 0 || high_pass_results->count > OBJ_NUMB_MAX_SIZE ||
        low_pass_results->count < 0 || low_pass_results->count > OBJ_NUMB_MAX_SIZE)
    {
        return -1;
    }

    *views = DetectionResultViews();
    views->low_threshold = low_threshold;
    views->high_threshold = high_threshold;
    views->candidate_count = low_pass_results->count;
    views->high_pass_count = high_pass_results->count;
    views->low_pass_count = low_pass_results->count;
    views->tracker_candidates.id = high_pass_results->id;
    views->display_detections = *high_pass_results;

    for (int i = 0; i < high_pass_results->count; ++i)
    {
        views->tracker_candidates.results[views->tracker_candidates.count++] = high_pass_results->results[i];
    }

    for (int i = 0; i < low_pass_results->count; ++i)
    {
        const detect_result_t &result = low_pass_results->results[i];
        if (result.prop < low_threshold)
        {
            views->below_low_count++;
            continue;
        }
        if (result.prop >= high_threshold)
        {
            if (overlaps_high_pass(result, *high_pass_results, overlap_threshold))
            {
                views->low_pass_high_duplicate_count++;
            }
            else
            {
                views->rejected_low_pass_high_score_count++;
            }
            continue;
        }
        if (overlaps_high_pass(result, *high_pass_results, overlap_threshold))
        {
            views->suppressed_low_only_overlap_count++;
            continue;
        }
        if (views->tracker_candidates.count >= OBJ_NUMB_MAX_SIZE)
        {
            views->capacity_dropped_count++;
            continue;
        }
        views->tracker_candidates.results[views->tracker_candidates.count++] = result;
        views->low_only_count++;
    }
    return 0;
}
