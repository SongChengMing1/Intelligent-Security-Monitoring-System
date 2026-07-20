#ifndef STREAM_OVERLAY_POLICY_H
#define STREAM_OVERLAY_POLICY_H

#include <string>
#include <vector>

struct StreamFrameMetadata
{
    long long capture_frame_id;
    long long capture_time_ms;

    StreamFrameMetadata()
        : capture_frame_id(0), capture_time_ms(0)
    {
    }

    StreamFrameMetadata(long long frame_id, long long time_ms)
        : capture_frame_id(frame_id), capture_time_ms(time_ms)
    {
    }
};

struct OverlayBox
{
    int left;
    int top;
    int right;
    int bottom;
    float score;
    std::string name;

    OverlayBox()
        : left(0), top(0), right(0), bottom(0), score(0.0f), name()
    {
    }
};

struct DetectionOverlayResult
{
    long long capture_frame_id;
    long long capture_time_ms;
    long long update_time_ms;
    std::vector<OverlayBox> boxes;

    DetectionOverlayResult()
        : capture_frame_id(0), capture_time_ms(0), update_time_ms(0), boxes()
    {
    }
};

struct OverlaySelection
{
    std::string state;
    bool draw;
    int lag_frames;
    long long lag_ms;
    long long detection_capture_frame_id;
    long long detection_capture_time_ms;
    long long detection_update_time_ms;
    std::vector<OverlayBox> boxes;

    OverlaySelection()
        : state("none"),
          draw(false),
          lag_frames(0),
          lag_ms(0),
          detection_capture_frame_id(0),
          detection_capture_time_ms(0),
          detection_update_time_ms(0),
          boxes()
    {
    }
};

OverlaySelection select_overlay_result_for_frame(
    const StreamFrameMetadata &video_frame,
    const std::vector<DetectionOverlayResult> &results,
    long long latest_hold_ms);

#endif // STREAM_OVERLAY_POLICY_H
