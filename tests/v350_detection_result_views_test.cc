#include "detection_result_views.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

detect_result_t make_result(int class_id, const char *name, float score, int offset)
{
    detect_result_t result;
    memset(&result, 0, sizeof(result));
    result.class_id = class_id;
    strncpy(result.name, name, OBJ_NAME_MAX_SIZE - 1);
    result.prop = score;
    result.box.left = offset;
    result.box.top = offset + 1;
    result.box.right = offset + 20;
    result.box.bottom = offset + 40;
    return result;
}

void verify_threshold_partition()
{
    detect_result_group_t input;
    memset(&input, 0, sizeof(input));
    input.id = 17;
    input.count = 5;
    input.results[0] = make_result(0, "person", 0.05f, 0);
    input.results[1] = make_result(0, "person", 0.10f, 10);
    input.results[2] = make_result(2, "car", 0.49f, 20);
    input.results[3] = make_result(2, "car", 0.50f, 30);
    input.results[4] = make_result(7, "truck", 0.90f, 40);

    DetectionResultViews views;
    require(build_detection_result_views(&input, 0.10f, 0.50f, &views) == 0,
            "threshold partition failed");
    require(views.candidate_count == 5, "candidate count mismatch");
    require(views.below_low_count == 1, "below-low count mismatch");
    require(views.tracker_candidates.count == 4, "tracker candidate count mismatch");
    require(views.display_detections.count == 2, "display detection count mismatch");
    require(std::fabs(views.tracker_candidates.results[0].prop - 0.10f) < 0.0001f,
            "low threshold must be inclusive");
    require(std::fabs(views.display_detections.results[0].prop - 0.50f) < 0.0001f,
            "high threshold must be inclusive");
    require(views.tracker_candidates.results[1].class_id == 2,
            "class ID was not preserved in tracker view");
    require(std::string(views.display_detections.results[1].name) == "truck",
            "class name was not preserved in display view");
    require(views.tracker_candidates.id == input.id && views.display_detections.id == input.id,
            "group ID was not preserved");
}

void verify_bounds_and_invalid_input()
{
    detect_result_group_t input;
    memset(&input, 0, sizeof(input));
    input.count = OBJ_NUMB_MAX_SIZE;
    for (int i = 0; i < input.count; ++i)
    {
        input.results[i] = make_result(i % 3, "object", 0.75f, i);
    }

    DetectionResultViews views;
    require(build_detection_result_views(&input, 0.10f, 0.50f, &views) == 0,
            "maximum-size group was rejected");
    require(views.tracker_candidates.count == OBJ_NUMB_MAX_SIZE,
            "tracker view exceeded or truncated the valid hard capacity");
    require(views.display_detections.count == OBJ_NUMB_MAX_SIZE,
            "display view exceeded or truncated the valid hard capacity");

    require(build_detection_result_views(NULL, 0.10f, 0.50f, &views) < 0,
            "null input was accepted");
    require(build_detection_result_views(&input, 0.50f, 0.50f, &views) < 0,
            "equal thresholds were accepted");
    require(build_detection_result_views(&input, -0.10f, 0.50f, &views) < 0,
            "negative low threshold was accepted");
    require(build_detection_result_views(&input, 0.10f, 1.10f, &views) < 0,
            "high threshold above one was accepted");
    input.count = OBJ_NUMB_MAX_SIZE + 1;
    require(build_detection_result_views(&input, 0.10f, 0.50f, &views) < 0,
            "invalid source count above capacity was accepted");
}

void verify_dual_pass_preserves_high_results()
{
    detect_result_group_t high_pass;
    detect_result_group_t low_pass;
    memset(&high_pass, 0, sizeof(high_pass));
    memset(&low_pass, 0, sizeof(low_pass));
    high_pass.id = 31;
    high_pass.count = 1;
    high_pass.results[0] = make_result(0, "person", 0.72f, 100);

    low_pass.id = 31;
    low_pass.count = 4;
    low_pass.results[0] = make_result(11, "stop sign", 0.86f, 5);
    low_pass.results[1] = make_result(0, "person", 0.72f, 100);
    low_pass.results[2] = make_result(0, "person", 0.34f, 110);
    low_pass.results[3] = make_result(0, "person", 0.40f, 100);

    DetectionResultViews views;
    require(build_dual_pass_detection_result_views(&high_pass, &low_pass, 0.25f, 0.50f, 0.60f, &views) == 0,
            "dual-pass view construction failed");
    require(views.display_detections.count == 1,
            "low pass changed the raw high-pass detection count");
    require(std::string(views.display_detections.results[0].name) == "person",
            "low pass replaced the raw high-pass detection");
    require(views.tracker_candidates.count == 2,
            "tracker input must contain high-pass plus qualified low-only observations");
    require(std::fabs(views.tracker_candidates.results[1].prop - 0.34f) < 0.0001f,
            "qualified low-only observation was not retained");
    require(views.low_pass_high_duplicate_count == 1,
            "high-pass duplicate was not diagnosed");
    require(views.rejected_low_pass_high_score_count == 1,
            "low-pass high-score anomalies were not rejected");
    require(views.suppressed_low_only_overlap_count == 1,
            "overlapping low-only duplicate was not suppressed");
    require(views.high_pass_count == 1 && views.low_pass_count == 4 && views.low_only_count == 1,
            "dual-pass diagnostics mismatch");
}

void verify_dual_pass_prioritizes_high_capacity()
{
    detect_result_group_t high_pass;
    detect_result_group_t low_pass;
    memset(&high_pass, 0, sizeof(high_pass));
    memset(&low_pass, 0, sizeof(low_pass));
    high_pass.count = OBJ_NUMB_MAX_SIZE;
    low_pass.count = 1;
    for (int i = 0; i < high_pass.count; ++i)
    {
        high_pass.results[i] = make_result(0, "person", 0.75f, i);
    }
    low_pass.results[0] = make_result(0, "person", 0.30f, 200);

    DetectionResultViews views;
    require(build_dual_pass_detection_result_views(&high_pass, &low_pass, 0.25f, 0.50f, 0.60f, &views) == 0,
            "dual-pass capacity construction failed");
    require(views.display_detections.count == OBJ_NUMB_MAX_SIZE,
            "high-pass output was truncated");
    require(views.tracker_candidates.count == OBJ_NUMB_MAX_SIZE,
            "tracker capacity bound mismatch");
    require(views.capacity_dropped_count == 1 && views.low_only_count == 0,
            "low-only capacity drop was not diagnosed");
}
} // namespace

int main()
{
    verify_threshold_partition();
    verify_bounds_and_invalid_input();
    verify_dual_pass_preserves_high_results();
    verify_dual_pass_prioritizes_high_capacity();
    std::cout << "PASS v350_detection_result_views_test" << std::endl;
    return 0;
}
