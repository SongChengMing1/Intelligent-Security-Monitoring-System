#include "stream_overlay_policy.h"

static void fill_selection_from_result(OverlaySelection *selection,
                                       const StreamFrameMetadata &video_frame,
                                       const DetectionOverlayResult &result)
{
    selection->lag_frames = (int)(video_frame.capture_frame_id - result.capture_frame_id);
    selection->lag_ms = video_frame.capture_time_ms - result.capture_time_ms;
    selection->detection_capture_frame_id = result.capture_frame_id;
    selection->detection_capture_time_ms = result.capture_time_ms;
    selection->detection_update_time_ms = result.update_time_ms;
    selection->boxes = result.boxes;
}

OverlaySelection select_overlay_result_for_frame(
    const StreamFrameMetadata &video_frame,
    const std::vector<DetectionOverlayResult> &results,
    long long latest_hold_ms)
{
    OverlaySelection selection;

    if (video_frame.capture_frame_id <= 0 || video_frame.capture_time_ms <= 0)
    {
        selection.state = "unavailable";
        return selection;
    }
    if (results.empty())
    {
        selection.state = "none";
        return selection;
    }

    const DetectionOverlayResult *exact = NULL;
    const DetectionOverlayResult *nearest_previous = NULL;
    bool has_future_result = false;

    for (size_t i = 0; i < results.size(); ++i)
    {
        const DetectionOverlayResult &candidate = results[i];
        if (candidate.capture_frame_id <= 0 || candidate.capture_time_ms <= 0)
        {
            continue;
        }
        if (candidate.capture_frame_id == video_frame.capture_frame_id)
        {
            exact = &candidate;
            break;
        }
        if (candidate.capture_time_ms <= video_frame.capture_time_ms)
        {
            if (!nearest_previous ||
                candidate.capture_time_ms > nearest_previous->capture_time_ms ||
                (candidate.capture_time_ms == nearest_previous->capture_time_ms &&
                 candidate.capture_frame_id > nearest_previous->capture_frame_id))
            {
                nearest_previous = &candidate;
            }
        }
        else
        {
            has_future_result = true;
        }
    }

    if (exact)
    {
        selection.state = "matched";
        fill_selection_from_result(&selection, video_frame, *exact);
        selection.draw = !selection.boxes.empty();
        return selection;
    }

    if (nearest_previous)
    {
        fill_selection_from_result(&selection, video_frame, *nearest_previous);
        if (selection.lag_ms >= 0 && selection.lag_ms <= latest_hold_ms)
        {
            selection.state = "latest";
            selection.draw = !selection.boxes.empty();
        }
        else
        {
            selection.state = "stale";
            selection.draw = false;
        }
        return selection;
    }

    selection.state = has_future_result ? "stale" : "none";
    return selection;
}
