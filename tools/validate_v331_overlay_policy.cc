#include "stream_overlay_policy.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

static OverlayBox make_box()
{
    OverlayBox box;
    box.left = 10;
    box.top = 20;
    box.right = 100;
    box.bottom = 160;
    box.score = 0.9f;
    box.name = "person";
    return box;
}

static DetectionOverlayResult make_result(long long frame_id, long long time_ms, bool with_box)
{
    DetectionOverlayResult result;
    result.capture_frame_id = frame_id;
    result.capture_time_ms = time_ms;
    result.update_time_ms = time_ms + 100;
    if (with_box)
    {
        result.boxes.push_back(make_box());
    }
    return result;
}

static void expect_state(const char *case_name,
                         const OverlaySelection &selection,
                         const std::string &state,
                         bool draw,
                         long long detection_frame_id)
{
    if (selection.state != state || selection.draw != draw ||
        selection.detection_capture_frame_id != detection_frame_id)
    {
        std::cerr << case_name << " failed: state=" << selection.state
                  << " draw=" << (selection.draw ? "true" : "false")
                  << " detection_frame_id=" << selection.detection_capture_frame_id
                  << std::endl;
        std::exit(1);
    }
}

int main()
{
    const long long hold_ms = 300;

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(100, 1000, true));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1000), results, hold_ms);
        expect_state("matched", selection, "matched", true, 100);
    }

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(98, 940, true));
        results.push_back(make_result(99, 980, true));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1040), results, hold_ms);
        expect_state("latest", selection, "latest", true, 99);
    }

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(80, 500, true));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1000), results, hold_ms);
        expect_state("stale", selection, "stale", false, 80);
    }

    {
        std::vector<DetectionOverlayResult> results;
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1000), results, hold_ms);
        expect_state("none", selection, "none", false, 0);
    }

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(101, 1033, true));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1000), results, hold_ms);
        expect_state("future_rejected", selection, "stale", false, 0);
    }

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(100, 1000, false));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(100, 1000), results, hold_ms);
        expect_state("matched_empty_boxes", selection, "matched", false, 100);
    }

    {
        std::vector<DetectionOverlayResult> results;
        results.push_back(make_result(100, 1000, true));
        OverlaySelection selection =
            select_overlay_result_for_frame(StreamFrameMetadata(0, 0), results, hold_ms);
        expect_state("unavailable", selection, "unavailable", false, 0);
    }

    std::cout << "v331_overlay_policy_ok" << std::endl;
    return 0;
}
