#include "runtime_evidence.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sys/time.h>

namespace
{
const size_t kDetectionResultRingCapacity = 128;

struct RuntimeEvidenceData
{
    mutable std::mutex mutex;
    std::condition_variable frame_cv;
    cv::Mat latest_frame;
    std::deque<CapturedFrame> frame_ring;
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
    std::vector<OverlayBox> latest_detection_boxes;
    std::deque<DetectionOverlayResult> detection_result_ring;
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

    long long capture_rate_ms;
    long long capture_rate_frames;
    long long ai_rate_ms;
    long long ai_rate_frames;
    long long inference_rate_ms;
    long long inference_rate_frames;
    long long rtsp_rate_ms;
    long long rtsp_rate_frames;

    RuntimeEvidenceData()
        : latest_capture_frame_id(0),
          latest_capture_frame_time_ms(0),
          last_update_ms(0),
          captured_frames(0),
          rtsp_encoded_frames(0),
          rtsp_push_errors(0),
          rtsp_dropped_frames(0),
          ai_appsink_frames(0),
          ai_appsink_dropped_frames(0),
          ai_appsink_errors(0),
          frame_ring_dropped_frames(0),
          dropped_frames(0),
          inference_frames(0),
          inference_skipped_frames(0),
          latest_detection_frame_id(0),
          latest_detection_capture_frame_time_ms(0),
          latest_detection_time_ms(0),
          detection_count(0),
          tracker_candidate_count(0),
          display_detection_count(0),
          high_pass_detection_count(0),
          low_pass_detection_count(0),
          low_only_candidate_count(0),
          low_pass_high_duplicate_count(0),
          rejected_low_pass_high_score_count(0),
          suppressed_low_only_overlap_count(0),
          tracker_candidate_capacity_dropped_count(0),
          track_count(0),
          latest_track_frame_id(0),
          latest_track_capture_time_ms(0),
          latest_track_update_time_ms(0),
          tracker_diagnostics(),
          profile_admitted_total(0),
          profile_rejected_total(0),
          profile_admitted_last_frame(0),
          profile_rejected_last_frame(0),
          profile_rejected_by_reason(),
          last_policy_ms(0.0),
          average_policy_ms(0.0),
          max_policy_ms(0.0),
          latest_detection_boxes(),
          detection_result_ring(),
          capture_fps(0.0),
          rtsp_fps(0.0),
          inference_fps(0.0),
          last_preprocess_ms(0.0),
          last_inference_ms(0.0),
          last_postprocess_ms(0.0),
          latest_preprocess_transform(),
          overlay_latency_ms(0.0),
          latest_rtsp_capture_frame_id(0),
          latest_rtsp_capture_frame_time_ms(0),
          latest_rtsp_push_time_ms(0),
          latest_rtsp_overlay_lag_frames(0),
          latest_rtsp_overlay_lag_ms(0),
          latest_rtsp_overlay_detection_frame_id(0),
          latest_rtsp_overlay_detection_capture_time_ms(0),
          latest_rtsp_overlay_detection_update_time_ms(0),
          latest_rtsp_overlay_drawn(false),
          latest_rtsp_alignment_state("unavailable"),
          local_display_running(false),
          local_display_sink("none"),
          rtsp_running(false),
          rtsp_ready(false),
          camera_opened(false),
          capture_running(false),
          inference_running(false),
          inference_ready(false),
          latest_detection_json("{\"state\":\"starting\",\"model_type\":\"yolov6\",\"schema_version\":\"v4.0.0\",\"result_type\":\"detection\",\"tracking_state\":\"pass_through\",\"capture_frame_id\":0,\"capture_timestamp_ms\":0,\"update_time_ms\":0,\"latency_ms\":0,\"source_width\":0,\"source_height\":0,\"detection_count\":0,\"objects\":[]}"),
          latest_track_json("{\"state\":\"starting\",\"schema_version\":\"v4.0.0\",\"result_type\":\"tracking\",\"tracker_type\":\"none\",\"capture_frame_id\":0,\"capture_timestamp_ms\":0,\"update_time_ms\":0,\"source_width\":0,\"source_height\":0,\"track_count\":0,\"objects\":[]}"),
          latest_intrusion_json("{\"schema_version\":1,\"runtime_session_id\":\"\",\"state\":\"disabled\",\"enabled\":false,\"event_sequence\":0,\"in_region_targets\":[],\"active_alarms\":[],\"recent_events\":[]}"),
          tracker_state("starting"),
          tracker_error(),
          intrusion_state("disabled"),
          intrusion_error(),
          recorder_diagnostics(),
          state("starting"),
          message("initializing"),
          capture_rate_ms(0),
          capture_rate_frames(0),
          ai_rate_ms(0),
          ai_rate_frames(0),
          inference_rate_ms(0),
          inference_rate_frames(0),
          rtsp_rate_ms(0),
          rtsp_rate_frames(0)
    {
    }
};

static void update_rate(long long now_ms, long long *last_rate_ms, long long *last_rate_frames,
                        long long current_frames, double *rate)
{
    if (*last_rate_ms == 0)
    {
        *last_rate_ms = now_ms;
        *last_rate_frames = current_frames;
        return;
    }
    if (now_ms - *last_rate_ms < 1000)
    {
        return;
    }
    const double seconds = (now_ms - *last_rate_ms) / 1000.0;
    *rate = (current_frames - *last_rate_frames) / seconds;
    *last_rate_frames = current_frames;
    *last_rate_ms = now_ms;
}

static long long current_time_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<long long>(tv.tv_sec) * 1000LL + tv.tv_usec / 1000;
}

static void copy_snapshot(const RuntimeEvidenceData &data, RuntimeEvidenceSnapshot *snapshot)
{
    snapshot->frame_ring_size = data.frame_ring.size();
    snapshot->latest_detection_boxes = data.latest_detection_boxes;
    snapshot->detection_result_ring = data.detection_result_ring;
    snapshot->latest_capture_frame_id = data.latest_capture_frame_id;
    snapshot->latest_capture_frame_time_ms = data.latest_capture_frame_time_ms;
    snapshot->last_update_ms = data.last_update_ms;
    snapshot->captured_frames = data.captured_frames;
    snapshot->rtsp_encoded_frames = data.rtsp_encoded_frames;
    snapshot->rtsp_push_errors = data.rtsp_push_errors;
    snapshot->rtsp_dropped_frames = data.rtsp_dropped_frames;
    snapshot->ai_appsink_frames = data.ai_appsink_frames;
    snapshot->ai_appsink_dropped_frames = data.ai_appsink_dropped_frames;
    snapshot->ai_appsink_errors = data.ai_appsink_errors;
    snapshot->frame_ring_dropped_frames = data.frame_ring_dropped_frames;
    snapshot->dropped_frames = data.dropped_frames;
    snapshot->inference_frames = data.inference_frames;
    snapshot->inference_skipped_frames = data.inference_skipped_frames;
    snapshot->latest_detection_frame_id = data.latest_detection_frame_id;
    snapshot->latest_detection_capture_frame_time_ms = data.latest_detection_capture_frame_time_ms;
    snapshot->latest_detection_time_ms = data.latest_detection_time_ms;
    snapshot->detection_count = data.detection_count;
    snapshot->tracker_candidate_count = data.tracker_candidate_count;
    snapshot->display_detection_count = data.display_detection_count;
    snapshot->high_pass_detection_count = data.high_pass_detection_count;
    snapshot->low_pass_detection_count = data.low_pass_detection_count;
    snapshot->low_only_candidate_count = data.low_only_candidate_count;
    snapshot->low_pass_high_duplicate_count = data.low_pass_high_duplicate_count;
    snapshot->rejected_low_pass_high_score_count = data.rejected_low_pass_high_score_count;
    snapshot->suppressed_low_only_overlap_count = data.suppressed_low_only_overlap_count;
    snapshot->tracker_candidate_capacity_dropped_count = data.tracker_candidate_capacity_dropped_count;
    snapshot->track_count = data.track_count;
    snapshot->latest_track_frame_id = data.latest_track_frame_id;
    snapshot->latest_track_capture_time_ms = data.latest_track_capture_time_ms;
    snapshot->latest_track_update_time_ms = data.latest_track_update_time_ms;
    snapshot->tracker_diagnostics = data.tracker_diagnostics;
    snapshot->profile_admitted_total = data.profile_admitted_total;
    snapshot->profile_rejected_total = data.profile_rejected_total;
    snapshot->profile_admitted_last_frame = data.profile_admitted_last_frame;
    snapshot->profile_rejected_last_frame = data.profile_rejected_last_frame;
    snapshot->profile_rejected_by_reason = data.profile_rejected_by_reason;
    snapshot->last_policy_ms = data.last_policy_ms;
    snapshot->average_policy_ms = data.average_policy_ms;
    snapshot->max_policy_ms = data.max_policy_ms;
    snapshot->capture_fps = data.capture_fps;
    snapshot->rtsp_fps = data.rtsp_fps;
    snapshot->inference_fps = data.inference_fps;
    snapshot->last_preprocess_ms = data.last_preprocess_ms;
    snapshot->last_inference_ms = data.last_inference_ms;
    snapshot->last_postprocess_ms = data.last_postprocess_ms;
    snapshot->latest_preprocess_transform = data.latest_preprocess_transform;
    snapshot->overlay_latency_ms = data.overlay_latency_ms;
    snapshot->latest_rtsp_capture_frame_id = data.latest_rtsp_capture_frame_id;
    snapshot->latest_rtsp_capture_frame_time_ms = data.latest_rtsp_capture_frame_time_ms;
    snapshot->latest_rtsp_push_time_ms = data.latest_rtsp_push_time_ms;
    snapshot->latest_rtsp_overlay_lag_frames = data.latest_rtsp_overlay_lag_frames;
    snapshot->latest_rtsp_overlay_lag_ms = data.latest_rtsp_overlay_lag_ms;
    snapshot->latest_rtsp_overlay_detection_frame_id = data.latest_rtsp_overlay_detection_frame_id;
    snapshot->latest_rtsp_overlay_detection_capture_time_ms = data.latest_rtsp_overlay_detection_capture_time_ms;
    snapshot->latest_rtsp_overlay_detection_update_time_ms = data.latest_rtsp_overlay_detection_update_time_ms;
    snapshot->latest_rtsp_overlay_drawn = data.latest_rtsp_overlay_drawn;
    snapshot->latest_rtsp_alignment_state = data.latest_rtsp_alignment_state;
    snapshot->local_display_running = data.local_display_running;
    snapshot->local_display_sink = data.local_display_sink;
    snapshot->rtsp_running = data.rtsp_running;
    snapshot->rtsp_ready = data.rtsp_ready;
    snapshot->camera_opened = data.camera_opened;
    snapshot->capture_running = data.capture_running;
    snapshot->inference_running = data.inference_running;
    snapshot->inference_ready = data.inference_ready;
    snapshot->latest_detection_json = data.latest_detection_json;
    snapshot->latest_track_json = data.latest_track_json;
    snapshot->latest_intrusion_json = data.latest_intrusion_json;
    snapshot->tracker_state = data.tracker_state;
    snapshot->tracker_error = data.tracker_error;
    snapshot->intrusion_state = data.intrusion_state;
    snapshot->intrusion_error = data.intrusion_error;
    snapshot->recorder_diagnostics = data.recorder_diagnostics;
    snapshot->state = data.state;
    snapshot->message = data.message;
}
}  // namespace

RuntimeEvidenceSnapshot::RuntimeEvidenceSnapshot()
    : frame_ring_size(0),
      latest_detection_boxes(),
      detection_result_ring(),
      latest_capture_frame_id(0),
      latest_capture_frame_time_ms(0),
      last_update_ms(0),
      captured_frames(0),
      rtsp_encoded_frames(0),
      rtsp_push_errors(0),
      rtsp_dropped_frames(0),
      ai_appsink_frames(0),
      ai_appsink_dropped_frames(0),
      ai_appsink_errors(0),
      frame_ring_dropped_frames(0),
      dropped_frames(0),
      inference_frames(0),
      inference_skipped_frames(0),
      latest_detection_frame_id(0),
      latest_detection_capture_frame_time_ms(0),
      latest_detection_time_ms(0),
      detection_count(0),
      tracker_candidate_count(0),
      display_detection_count(0),
      high_pass_detection_count(0),
      low_pass_detection_count(0),
      low_only_candidate_count(0),
      low_pass_high_duplicate_count(0),
      rejected_low_pass_high_score_count(0),
      suppressed_low_only_overlap_count(0),
      tracker_candidate_capacity_dropped_count(0),
      track_count(0),
      latest_track_frame_id(0),
      latest_track_capture_time_ms(0),
      latest_track_update_time_ms(0),
      tracker_diagnostics(),
      profile_admitted_total(0),
      profile_rejected_total(0),
      profile_admitted_last_frame(0),
      profile_rejected_last_frame(0),
      profile_rejected_by_reason(),
      last_policy_ms(0.0),
      average_policy_ms(0.0),
      max_policy_ms(0.0),
      capture_fps(0.0),
      rtsp_fps(0.0),
      inference_fps(0.0),
      last_preprocess_ms(0.0),
      last_inference_ms(0.0),
      last_postprocess_ms(0.0),
      latest_preprocess_transform(),
      overlay_latency_ms(0.0),
      latest_rtsp_capture_frame_id(0),
      latest_rtsp_capture_frame_time_ms(0),
      latest_rtsp_push_time_ms(0),
      latest_rtsp_overlay_lag_frames(0),
      latest_rtsp_overlay_lag_ms(0),
      latest_rtsp_overlay_detection_frame_id(0),
      latest_rtsp_overlay_detection_capture_time_ms(0),
      latest_rtsp_overlay_detection_update_time_ms(0),
      latest_rtsp_overlay_drawn(false),
      latest_rtsp_alignment_state("unavailable"),
      local_display_running(false),
      local_display_sink("none"),
      rtsp_running(false),
      rtsp_ready(false),
      camera_opened(false),
      capture_running(false),
      inference_running(false),
      inference_ready(false),
      latest_detection_json(),
      latest_track_json(),
      latest_intrusion_json(),
      tracker_state(),
      tracker_error(),
      intrusion_state(),
      intrusion_error(),
      recorder_diagnostics(),
      state(),
      message()
{
}

RuntimeEvidenceAnalysisPublication::RuntimeEvidenceAnalysisPublication()
    : frame_id(0),
      frame_time_ms(0),
      update_ms(0),
      detection_boxes(),
      overlay_result(),
      detection_count(0),
      tracker_candidate_count(0),
      display_detection_count(0),
      high_pass_detection_count(0),
      low_pass_detection_count(0),
      low_only_candidate_count(0),
      low_pass_high_duplicate_count(0),
      rejected_low_pass_high_score_count(0),
      suppressed_low_only_overlap_count(0),
      tracker_candidate_capacity_dropped_count(0),
      track_count(0),
      latest_track_frame_id(0),
      latest_track_capture_time_ms(0),
      tracker_diagnostics(),
      profile_admitted_last_frame(0),
      profile_rejected_last_frame(0),
      profile_rejected_by_reason(),
      policy_ms(0.0),
      tracker_state(),
      tracker_error(),
      intrusion_state(),
      intrusion_error(),
      detection_json(),
      track_json(),
      intrusion_json(),
      preprocess_ms(0.0),
      inference_ms(0.0),
      postprocess_ms(0.0),
      preprocess_transform(),
      overlay_latency_ms(0.0),
      has_recorder_diagnostics(false),
      recorder_diagnostics()
{
}

class RuntimeEvidence::Impl
{
public:
    RuntimeEvidenceData data;
};

RuntimeEvidence::RuntimeEvidence()
    : impl_(new Impl())
{
}

RuntimeEvidence::~RuntimeEvidence()
{
}

RuntimeEvidenceSnapshot RuntimeEvidence::snapshot() const
{
    RuntimeEvidenceSnapshot result;
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    copy_snapshot(impl_->data, &result);
    return result;
}

void RuntimeEvidence::set_message(const std::string &state, const std::string &message, long long update_ms)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.state = state;
    impl_->data.message = message;
    impl_->data.last_update_ms = update_ms;
}

void RuntimeEvidence::initialize_gstreamer_state(bool local_display_running,
                                                  const std::string &local_display_sink,
                                                  const std::string &message,
                                                  const std::string &detection_json,
                                                  const std::string &tracker_state,
                                                  const std::string &tracker_error,
                                                  const std::string &track_json,
                                                  long long update_ms)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.camera_opened = false;
    impl_->data.capture_running = false;
    impl_->data.inference_running = false;
    impl_->data.inference_ready = false;
    impl_->data.local_display_running = local_display_running;
    impl_->data.local_display_sink = local_display_sink;
    impl_->data.state = "running";
    impl_->data.message = message;
    impl_->data.latest_detection_json = detection_json;
    impl_->data.tracker_state = tracker_state;
    impl_->data.tracker_error = tracker_error;
    impl_->data.latest_track_json = track_json;
    impl_->data.last_update_ms = update_ms;
}

void RuntimeEvidence::initialize_intrusion_state(const std::string &state,
                                                  const std::string &error,
                                                  const std::string &intrusion_json,
                                                  long long update_ms)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.intrusion_state = state;
    impl_->data.intrusion_error = error;
    impl_->data.latest_intrusion_json = intrusion_json;
    impl_->data.last_update_ms = update_ms;
}

void RuntimeEvidence::set_inference_status(bool running, bool ready,
                                            const std::string &detection_json,
                                            const std::string &tracker_state,
                                            const std::string &tracker_error,
                                            const std::string &track_json,
                                            const std::string &intrusion_state,
                                            const std::string &intrusion_error,
                                            const std::string &intrusion_json,
                                            long long update_ms,
                                            const std::string &state,
                                            const std::string &message)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.inference_running = running;
    impl_->data.inference_ready = ready;
    impl_->data.latest_detection_json = detection_json;
    impl_->data.tracker_state = tracker_state;
    impl_->data.tracker_error = tracker_error;
    impl_->data.latest_track_json = track_json;
    impl_->data.intrusion_state = intrusion_state;
    impl_->data.intrusion_error = intrusion_error;
    impl_->data.latest_intrusion_json = intrusion_json;
    if (!state.empty())
    {
        impl_->data.state = state;
    }
    if (!message.empty())
    {
        impl_->data.message = message;
    }
    impl_->data.last_update_ms = update_ms;
}

void RuntimeEvidence::finish_inference()
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.inference_running = false;
    impl_->data.inference_ready = false;
    if (impl_->data.state != "error")
    {
        impl_->data.last_update_ms = current_time_ms();
    }
}

int RuntimeEvidence::publish_captured_frame(const cv::Mat &frame, long long capture_time_ms,
                                             int width, int height, size_t frame_ring_capacity,
                                             bool from_ai_appsink)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    if (from_ai_appsink)
    {
        ++impl_->data.ai_appsink_frames;
    }
    ++impl_->data.captured_frames;
    impl_->data.latest_capture_frame_id = static_cast<int>(impl_->data.captured_frames);
    impl_->data.latest_capture_frame_time_ms = capture_time_ms;
    impl_->data.latest_frame = frame;

    CapturedFrame ring_frame;
    ring_frame.frame = frame;
    ring_frame.capture_frame_id = impl_->data.latest_capture_frame_id;
    ring_frame.capture_time_ms = capture_time_ms;
    ring_frame.width = width;
    ring_frame.height = height;
    impl_->data.frame_ring.push_back(ring_frame);
    while (impl_->data.frame_ring.size() > frame_ring_capacity)
    {
        impl_->data.frame_ring.pop_front();
        ++impl_->data.frame_ring_dropped_frames;
        if (from_ai_appsink)
        {
            ++impl_->data.ai_appsink_dropped_frames;
        }
    }
    impl_->data.last_update_ms = capture_time_ms;
    if (from_ai_appsink)
    {
        update_rate(capture_time_ms, &impl_->data.ai_rate_ms, &impl_->data.ai_rate_frames,
                    impl_->data.ai_appsink_frames, &impl_->data.capture_fps);
    }
    else
    {
        update_rate(capture_time_ms, &impl_->data.capture_rate_ms, &impl_->data.capture_rate_frames,
                    impl_->data.captured_frames, &impl_->data.capture_fps);
    }
    impl_->data.frame_cv.notify_all();
    return impl_->data.latest_capture_frame_id;
}

void RuntimeEvidence::mark_capture_started()
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.camera_opened = true;
    impl_->data.capture_running = true;
    impl_->data.state = "running";
    impl_->data.message = "capture running";
    impl_->data.last_update_ms = current_time_ms();
}

void RuntimeEvidence::mark_capture_stopped(bool stopped_by_signal)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.capture_running = false;
    impl_->data.state = impl_->data.state == "error" ? "error" : "stopped";
    impl_->data.message = stopped_by_signal ? "stopped by signal" : "capture stopped";
    impl_->data.last_update_ms = current_time_ms();
    impl_->data.frame_cv.notify_all();
}

void RuntimeEvidence::mark_pipeline_stopped(bool stopped_by_signal)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.capture_running = false;
    impl_->data.camera_opened = false;
    impl_->data.state = impl_->data.state == "error" ? "error" : "stopped";
    impl_->data.message = stopped_by_signal ? "stopped by signal" : "stopped";
    impl_->data.last_update_ms = current_time_ms();
}

bool RuntimeEvidence::copy_latest_frame(cv::Mat *frame, int *frame_id) const
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    if (impl_->data.latest_frame.empty())
    {
        return false;
    }
    *frame = impl_->data.latest_frame.clone();
    *frame_id = impl_->data.latest_capture_frame_id;
    return true;
}

bool RuntimeEvidence::wait_for_inference_frame(int last_processed_frame_id, cv::Mat *frame,
                                                int *frame_id, long long *frame_time_ms,
                                                bool stop_requested)
{
    std::unique_lock<std::mutex> lock(impl_->data.mutex);
    impl_->data.frame_cv.wait_for(lock, std::chrono::milliseconds(1000), [&]() {
        return stop_requested || (!impl_->data.latest_frame.empty() &&
                                  impl_->data.latest_capture_frame_id != last_processed_frame_id);
    });
    if (stop_requested || impl_->data.latest_frame.empty() ||
        impl_->data.latest_capture_frame_id == last_processed_frame_id)
    {
        return false;
    }
    *frame = impl_->data.latest_frame.clone();
    *frame_id = impl_->data.latest_capture_frame_id;
    *frame_time_ms = impl_->data.latest_capture_frame_time_ms;
    if (*frame_id > last_processed_frame_id + 1)
    {
        impl_->data.inference_skipped_frames += *frame_id - last_processed_frame_id - 1;
    }
    return true;
}

void RuntimeEvidence::notify_waiters()
{
    impl_->data.frame_cv.notify_all();
}

std::string RuntimeEvidence::latest_detection_json() const
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    return impl_->data.latest_detection_json;
}

std::string RuntimeEvidence::latest_track_json() const
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    return impl_->data.latest_track_json;
}

std::string RuntimeEvidence::latest_intrusion_json() const
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    return impl_->data.latest_intrusion_json;
}

void RuntimeEvidence::publish_analysis(const RuntimeEvidenceAnalysisPublication &publication)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.latest_detection_boxes = publication.detection_boxes;
    impl_->data.detection_result_ring.push_back(publication.overlay_result);
    while (impl_->data.detection_result_ring.size() > kDetectionResultRingCapacity)
    {
        impl_->data.detection_result_ring.pop_front();
    }
    ++impl_->data.inference_frames;
    impl_->data.latest_detection_frame_id = publication.frame_id;
    impl_->data.latest_detection_capture_frame_time_ms = publication.frame_time_ms;
    impl_->data.latest_detection_time_ms = publication.update_ms;
    impl_->data.detection_count = publication.detection_count;
    impl_->data.tracker_candidate_count = publication.tracker_candidate_count;
    impl_->data.display_detection_count = publication.display_detection_count;
    impl_->data.high_pass_detection_count = publication.high_pass_detection_count;
    impl_->data.low_pass_detection_count = publication.low_pass_detection_count;
    impl_->data.low_only_candidate_count = publication.low_only_candidate_count;
    impl_->data.low_pass_high_duplicate_count = publication.low_pass_high_duplicate_count;
    impl_->data.rejected_low_pass_high_score_count = publication.rejected_low_pass_high_score_count;
    impl_->data.suppressed_low_only_overlap_count = publication.suppressed_low_only_overlap_count;
    impl_->data.tracker_candidate_capacity_dropped_count = publication.tracker_candidate_capacity_dropped_count;
    impl_->data.track_count = publication.track_count;
    impl_->data.latest_track_frame_id = publication.latest_track_frame_id;
    impl_->data.latest_track_capture_time_ms = publication.latest_track_capture_time_ms;
    impl_->data.latest_track_update_time_ms = publication.update_ms;
    impl_->data.tracker_diagnostics = publication.tracker_diagnostics;
    impl_->data.profile_admitted_last_frame = publication.profile_admitted_last_frame;
    impl_->data.profile_rejected_last_frame = publication.profile_rejected_last_frame;
    impl_->data.profile_admitted_total += publication.profile_admitted_last_frame;
    impl_->data.profile_rejected_total += publication.profile_rejected_last_frame;
    for (std::map<std::string, long long>::const_iterator it = publication.profile_rejected_by_reason.begin();
         it != publication.profile_rejected_by_reason.end(); ++it)
    {
        impl_->data.profile_rejected_by_reason[it->first] += it->second;
    }
    impl_->data.last_policy_ms = publication.policy_ms;
    const long long policy_updates = impl_->data.inference_frames;
    impl_->data.average_policy_ms = policy_updates > 0
        ? ((impl_->data.average_policy_ms * (policy_updates - 1)) + publication.policy_ms) / policy_updates
        : publication.policy_ms;
    impl_->data.max_policy_ms = std::max(impl_->data.max_policy_ms, publication.policy_ms);
    impl_->data.tracker_state = publication.tracker_state;
    impl_->data.tracker_error = publication.tracker_error;
    impl_->data.intrusion_state = publication.intrusion_state;
    impl_->data.intrusion_error = publication.intrusion_error;
    impl_->data.latest_intrusion_json = publication.intrusion_json;
    if (publication.has_recorder_diagnostics)
    {
        impl_->data.recorder_diagnostics = publication.recorder_diagnostics;
    }
    impl_->data.latest_track_json = publication.track_json;
    impl_->data.last_preprocess_ms = publication.preprocess_ms;
    impl_->data.last_inference_ms = publication.inference_ms;
    impl_->data.last_postprocess_ms = publication.postprocess_ms;
    impl_->data.latest_preprocess_transform = publication.preprocess_transform;
    impl_->data.overlay_latency_ms = publication.overlay_latency_ms;
    impl_->data.latest_detection_json = publication.detection_json;
    impl_->data.last_update_ms = publication.update_ms;
    update_rate(publication.update_ms, &impl_->data.inference_rate_ms,
                &impl_->data.inference_rate_frames, impl_->data.inference_frames,
                &impl_->data.inference_fps);
}

void RuntimeEvidence::set_recorder_diagnostics(const RecorderDiagnostics &diagnostics)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.recorder_diagnostics = diagnostics;
}

void RuntimeEvidence::set_rtsp_server_state(bool running)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.rtsp_running = running;
    impl_->data.rtsp_ready = false;
}

void RuntimeEvidence::mark_rtsp_media_unprepared()
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.rtsp_ready = false;
    impl_->data.camera_opened = false;
    impl_->data.capture_running = false;
}

void RuntimeEvidence::mark_rtsp_ready(bool ready, const std::string &message)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    impl_->data.rtsp_ready = ready;
    if (!message.empty())
    {
        impl_->data.message = message;
        impl_->data.last_update_ms = current_time_ms();
    }
}

void RuntimeEvidence::record_rtsp_error(const std::string &message)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    ++impl_->data.rtsp_push_errors;
    impl_->data.rtsp_ready = false;
    if (!message.empty())
    {
        impl_->data.message = message;
        impl_->data.last_update_ms = current_time_ms();
    }
}

void RuntimeEvidence::record_ai_appsink_error(const std::string &message)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    ++impl_->data.ai_appsink_errors;
    if (!message.empty())
    {
        impl_->data.message = message;
        impl_->data.last_update_ms = current_time_ms();
    }
}

void RuntimeEvidence::publish_clean_rtsp_frame(long long now_ms, const std::string &message)
{
    std::lock_guard<std::mutex> lock(impl_->data.mutex);
    ++impl_->data.rtsp_encoded_frames;
    impl_->data.latest_rtsp_capture_frame_id = impl_->data.rtsp_encoded_frames;
    impl_->data.latest_rtsp_capture_frame_time_ms = 0;
    impl_->data.latest_rtsp_push_time_ms = now_ms;
    impl_->data.latest_rtsp_alignment_state = "clean";
    impl_->data.latest_rtsp_overlay_lag_frames = 0;
    impl_->data.latest_rtsp_overlay_lag_ms = 0;
    impl_->data.latest_rtsp_overlay_detection_frame_id = 0;
    impl_->data.latest_rtsp_overlay_detection_capture_time_ms = 0;
    impl_->data.latest_rtsp_overlay_detection_update_time_ms = 0;
    impl_->data.latest_rtsp_overlay_drawn = false;
    impl_->data.rtsp_ready = true;
    impl_->data.camera_opened = true;
    impl_->data.capture_running = true;
    impl_->data.last_update_ms = now_ms;
    impl_->data.message = message;
    update_rate(now_ms, &impl_->data.rtsp_rate_ms, &impl_->data.rtsp_rate_frames,
                impl_->data.rtsp_encoded_frames, &impl_->data.rtsp_fps);
}
