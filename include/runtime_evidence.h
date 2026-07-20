#ifndef RKNN_DETECT_RUNTIME_EVIDENCE_H_
#define RKNN_DETECT_RUNTIME_EVIDENCE_H_

#include <stddef.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "analysis_recorder.h"
#include "object_tracker.h"
#include "preprocess_transform.h"
#include "stream_overlay_policy.h"

struct CapturedFrame
{
    cv::Mat frame;
    long long capture_frame_id;
    long long capture_time_ms;
    int width;
    int height;

    CapturedFrame()
        : frame(), capture_frame_id(0), capture_time_ms(0), width(0), height(0)
    {
    }
};

// A read-only point-in-time view. HTTP projections consume this type instead
// of observing the producer-owned synchronization primitives directly.
struct RuntimeEvidenceSnapshot
{
    size_t frame_ring_size;
    std::vector<OverlayBox> latest_detection_boxes;
    std::deque<DetectionOverlayResult> detection_result_ring;
    int latest_capture_frame_id;
    long long latest_capture_frame_time_ms;
    long long last_update_ms;
    long long captured_frames;
    long long rtsp_encoded_frames;
    long long rtsp_push_errors;
    long long rtsp_dropped_frames;
    long long ai_appsink_frames;
    long long ai_appsink_dropped_frames;
    long long ai_appsink_errors;
    long long frame_ring_dropped_frames;
    long long dropped_frames;
    long long inference_frames;
    long long inference_skipped_frames;
    int latest_detection_frame_id;
    long long latest_detection_capture_frame_time_ms;
    long long latest_detection_time_ms;
    int detection_count;
    int tracker_candidate_count;
    int display_detection_count;
    int high_pass_detection_count;
    int low_pass_detection_count;
    int low_only_candidate_count;
    int low_pass_high_duplicate_count;
    int rejected_low_pass_high_score_count;
    int suppressed_low_only_overlap_count;
    int tracker_candidate_capacity_dropped_count;
    int track_count;
    int latest_track_frame_id;
    long long latest_track_capture_time_ms;
    long long latest_track_update_time_ms;
    TrackerDiagnostics tracker_diagnostics;
    long long profile_admitted_total;
    long long profile_rejected_total;
    int profile_admitted_last_frame;
    int profile_rejected_last_frame;
    std::map<std::string, long long> profile_rejected_by_reason;
    double last_policy_ms;
    double average_policy_ms;
    double max_policy_ms;
    double capture_fps;
    double rtsp_fps;
    double inference_fps;
    double last_preprocess_ms;
    double last_inference_ms;
    double last_postprocess_ms;
    preprocess_transform_t latest_preprocess_transform;
    double overlay_latency_ms;
    long long latest_rtsp_capture_frame_id;
    long long latest_rtsp_capture_frame_time_ms;
    long long latest_rtsp_push_time_ms;
    int latest_rtsp_overlay_lag_frames;
    long long latest_rtsp_overlay_lag_ms;
    long long latest_rtsp_overlay_detection_frame_id;
    long long latest_rtsp_overlay_detection_capture_time_ms;
    long long latest_rtsp_overlay_detection_update_time_ms;
    bool latest_rtsp_overlay_drawn;
    std::string latest_rtsp_alignment_state;
    bool local_display_running;
    std::string local_display_sink;
    bool rtsp_running;
    bool rtsp_ready;
    bool camera_opened;
    bool capture_running;
    bool inference_running;
    bool inference_ready;
    std::string latest_detection_json;
    std::string latest_track_json;
    std::string latest_intrusion_json;
    std::string tracker_state;
    std::string tracker_error;
    std::string intrusion_state;
    std::string intrusion_error;
    RecorderDiagnostics recorder_diagnostics;
    std::string state;
    std::string message;

    RuntimeEvidenceSnapshot();
};

struct RuntimeEvidenceAnalysisPublication
{
    int frame_id;
    long long frame_time_ms;
    long long update_ms;
    std::vector<OverlayBox> detection_boxes;
    DetectionOverlayResult overlay_result;
    int detection_count;
    int tracker_candidate_count;
    int display_detection_count;
    int high_pass_detection_count;
    int low_pass_detection_count;
    int low_only_candidate_count;
    int low_pass_high_duplicate_count;
    int rejected_low_pass_high_score_count;
    int suppressed_low_only_overlap_count;
    int tracker_candidate_capacity_dropped_count;
    int track_count;
    int latest_track_frame_id;
    long long latest_track_capture_time_ms;
    TrackerDiagnostics tracker_diagnostics;
    int profile_admitted_last_frame;
    int profile_rejected_last_frame;
    std::map<std::string, long long> profile_rejected_by_reason;
    double policy_ms;
    std::string tracker_state;
    std::string tracker_error;
    std::string intrusion_state;
    std::string intrusion_error;
    std::string detection_json;
    std::string track_json;
    std::string intrusion_json;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    preprocess_transform_t preprocess_transform;
    double overlay_latency_ms;
    bool has_recorder_diagnostics;
    RecorderDiagnostics recorder_diagnostics;

    RuntimeEvidenceAnalysisPublication();
};

class RuntimeEvidence
{
public:
    RuntimeEvidence();
    ~RuntimeEvidence();

    RuntimeEvidence(const RuntimeEvidence &) = delete;
    RuntimeEvidence &operator=(const RuntimeEvidence &) = delete;

    RuntimeEvidenceSnapshot snapshot() const;

    void set_message(const std::string &state, const std::string &message, long long update_ms);
    void initialize_gstreamer_state(bool local_display_running,
                                    const std::string &local_display_sink,
                                    const std::string &message,
                                    const std::string &detection_json,
                                    const std::string &tracker_state,
                                    const std::string &tracker_error,
                                    const std::string &track_json,
                                    long long update_ms);
    void initialize_intrusion_state(const std::string &state,
                                    const std::string &error,
                                    const std::string &intrusion_json,
                                    long long update_ms);
    void set_inference_status(bool running, bool ready,
                              const std::string &detection_json,
                              const std::string &tracker_state,
                              const std::string &tracker_error,
                              const std::string &track_json,
                              const std::string &intrusion_state,
                              const std::string &intrusion_error,
                              const std::string &intrusion_json,
                              long long update_ms,
                              const std::string &state = std::string(),
                              const std::string &message = std::string());
    void finish_inference();

    int publish_captured_frame(const cv::Mat &frame, long long capture_time_ms,
                               int width, int height, size_t frame_ring_capacity,
                               bool from_ai_appsink);
    void mark_capture_started();
    void mark_capture_stopped(bool stopped_by_signal);
    void mark_pipeline_stopped(bool stopped_by_signal);
    bool copy_latest_frame(cv::Mat *frame, int *frame_id) const;
    bool wait_for_inference_frame(int last_processed_frame_id, cv::Mat *frame,
                                  int *frame_id, long long *frame_time_ms,
                                  bool stop_requested);
    void notify_waiters();

    std::string latest_detection_json() const;
    std::string latest_track_json() const;
    std::string latest_intrusion_json() const;

    void publish_analysis(const RuntimeEvidenceAnalysisPublication &publication);
    void set_recorder_diagnostics(const RecorderDiagnostics &diagnostics);

    void set_rtsp_server_state(bool running);
    void mark_rtsp_media_unprepared();
    void mark_rtsp_ready(bool ready, const std::string &message = std::string());
    void record_rtsp_error(const std::string &message);
    void record_ai_appsink_error(const std::string &message);
    void publish_clean_rtsp_frame(long long now_ms, const std::string &message);


private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // RKNN_DETECT_RUNTIME_EVIDENCE_H_
