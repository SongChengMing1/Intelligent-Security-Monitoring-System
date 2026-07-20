#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <fstream>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "analysis_recorder.h"
#include "detection_result_views.h"
#include "detector.h"
#include "image_preprocess_opencv.h"
#include "intrusion_evaluator.h"
#include "local_overlay_renderer.h"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"
#include "object_tracker.h"
#include "observation_policy.h"
#include "perf_stats.h"
#include "postprocess.h"
#include "rknn_api.h"
#include "runtime_evidence.h"
#include "stream_pipeline.h"
#include "stream_overlay_policy.h"
#include "tracking_profile.h"
#include "sha256.h"

static volatile sig_atomic_t g_stop_requested = 0;
static const size_t kDetectionResultRingCapacity = 128;
static const int kSnapshotJpegQuality = 80;
static const char *kModelType = "yolov6";
static const char *kStreamSchemaVersion = "v4.0.0";

#ifndef RKNN_DETECT_GIT_COMMIT
#define RKNN_DETECT_GIT_COMMIT "unknown"
#endif

static std::string sha256_file(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return "unavailable";
    std::ostringstream content; content << input.rdbuf();
    return std::string("sha256:") + sha256_hex(content.str());
}

static bool safe_session_id(const std::string &value)
{
    if (value.empty()) return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (!(isalnum(static_cast<unsigned char>(value[i])) || value[i] == '-' || value[i] == '_' || value[i] == '.')) return false;
    return value != "." && value != "..";
}

enum LocalDisplayMode
{
    LOCAL_DISPLAY_DISABLED = 0,
    LOCAL_DISPLAY_FAKESINK = 1
};

enum StreamTrackerType
{
    STREAM_TRACKER_NONE = 0,
    STREAM_TRACKER_BYTETRACK = 1,
};

static void handle_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

struct StreamServerConfig
{
    int port;
    int max_seconds;
    std::string video_node;
    int width;
    int height;
    int fps;
    int inference_fps;
    int rtsp_port;
    std::string rtsp_path;
    int rtsp_fps;
    int h264_bitrate;
    LocalDisplayMode display_mode;
    int output_delay_ms;
    int latest_hold_ms;
    int frame_ring_size;
    int stale_threshold_ms;
    float box_conf_threshold;
    float nms_threshold;
    StreamTrackerType tracker_type;
    TrackerConfig tracker_config;
    std::string profile_id;
    std::string profile_hash;
    ObservationPolicyConfig observation_policy_config;
    int intrusion_schema_version;
    std::string intrusion_camera_id;
    IntrusionRuleConfig intrusion_config;
    bool intrusion_config_supplied;
    bool recording_enabled;
    RecordingSessionConfig recording_config;
    preprocess_mode_t preprocess_mode;
    std::string pixel_format;
    std::string model_path;
};


static void print_usage(const char *program)
{
    printf("Usage: %s [options] [rknn model]\n", program);
    printf("Options:\n");
    printf("  --port <N>              HTTP listen port, default 8080\n");
    printf("  --max-seconds <N>       stop after N seconds, default 0 means continuous\n");
    printf("  --video-node <path>     V4L2 video node, default /dev/video0\n");
    printf("  --width <value>         capture width, default 640\n");
    printf("  --height <value>        capture height, default 480\n");
    printf("  --fps <value>           capture fps, default 30\n");
    printf("  --inference-fps <value> RKNN inference sampling fps target, default 10\n");
    printf("  --rtsp-port <N>         RTSP listen port, default 8554\n");
    printf("  --rtsp-path <path>      RTSP mount path, default /rknn_detect\n");
    printf("  --rtsp-fps <value>      RTSP output fps target, default 25\n");
    printf("  --h264-bitrate <value>  H.264 target bitrate in bps, default 2000000\n");
    printf("  --display-mode <name>   local display branch: none or fakesink, default none\n");
    printf("  --output-delay-ms <N>   reserved RTSP alignment delay, default 250\n");
    printf("  --latest-hold-ms <N>    reserved latest detection hold duration, default 300\n");
    printf("  --frame-ring-size <N>   bounded captured frame ring size, default 60\n");
    printf("  --stale-threshold-ms <N> hide stale overlays after N ms, default 1000\n");
    printf("  --preprocess-mode <name> preprocess mode: resize or letterbox, default letterbox\n");
    printf("  --box-thresh <value>    detection box threshold, default 0.50\n");
    printf("  --nms-thresh <value>    detection NMS threshold, default 0.60\n");
    printf("  --tracker-type <name>   object tracker: none or bytetrack; GStreamer defaults to bytetrack\n");
    printf("  --tracker-low-thresh <value> low-score recovery threshold, stream default 0.35\n");
    printf("  --tracker-high-thresh <value> first association/display threshold, default box threshold\n");
    printf("  --tracker-new-track-thresh <value> new-track threshold, default high threshold\n");
    printf("  --tracker-match-thresh <value> first association cost limit, default 0.80\n");
    printf("  --tracker-second-match-thresh <value> low-score association cost limit, default 0.50\n");
    printf("  --tracker-confirm-hits <N> confirmations before normal overlay, default 2\n");
    printf("  --tracker-lost-timeout-ms <N> retain lost tracks for N ms, default 1000\n");
    printf("  --tracker-max-tracks <N> bounded tracker observation count, default 64\n");
    printf("  --profile-id <value> effective startup Profile ID, default default-general\n");
    printf("  --profile-hash <value> effective canonical Profile hash\n");
    printf("  --tracker-class-ids <csv> admitted class IDs; empty means all classes\n");
    printf("  --tracker-min-width <px> minimum admitted bbox width, default 0\n");
    printf("  --tracker-min-height <px> minimum admitted bbox height, default 0\n");
    printf("  --tracker-min-area <px2> minimum admitted bbox area, default 0\n");
    printf("  --tracker-edge-margin <px> reject bbox centers in source edge band, default 0\n");
    printf("  --tracker-roi <disabled|x1,y1,x2,y2> normalized center-point ROI\n");
    printf("  --intrusion-enabled       enable the explicit region intrusion event rule\n");
    printf("  --intrusion-disabled      explicitly disable the event rule\n");
    printf("  --intrusion-schema-version <N> event profile schema version, default 1\n");
    printf("  --intrusion-camera-id <id> logical camera ID, default camera0\n");
    printf("  --intrusion-rule-id <id>  event rule ID, default person-intrusion\n");
    printf("  --intrusion-class-ids <csv> event class IDs, default person class 0\n");
    printf("  --intrusion-region <x1,y1,x2,y2> normalized business rectangle\n");
    printf("  --intrusion-dwell-ms <N>  dwell threshold, default 5000\n");
    printf("  --intrusion-boundary-hysteresis-px <px> exit hysteresis, default 5\n");
    printf("  --intrusion-prediction-grace-ms <N> predicted gap grace, default 1000\n");
    printf("  --record-analysis       enable bounded metadata recording, default disabled\n");
    printf("  --recording-root <path> recording root, default recordings\n");
    printf("  --recording-session-id <id> explicit unique recording session ID\n");
    printf("  --runtime-session-id <id> explicit unique runtime session ID\n");
    printf("  --recording-queue-capacity <N> bounded record queue, default 128\n");
    printf("  --recording-segment-mb <N> segment limit, default 16 MiB\n");
    printf("  --recording-session-mb <N> session limit, default 1024 MiB\n");
    printf("  --recording-max-seconds <N> session duration limit, default 3600\n");
    printf("  --recording-min-free-mb <N> disk safety line, default 256 MiB\n");
    printf("  --record-frame-mode <none|sampled|all> image recording mode, default none\n");
    printf("  --record-jpeg-every <N> sampled AI-frame interval, default 10\n");
    printf("  --record-jpeg-quality <N> JPEG quality, default 80\n");
    printf("  --record-image-pool-capacity <N> bounded image payload pool, default 4\n");
    printf("  --pixel-format <fourcc> V4L2 FOURCC, default NV12\n");
}

static bool parse_int_arg(const char *name, const char *value, int min_value, int max_value, int *out)
{
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < min_value || parsed > max_value)
    {
        printf("%s must be an integer in [%d, %d], got '%s'\n", name, min_value, max_value, value);
        return false;
    }
    *out = (int)parsed;
    return true;
}

static bool parse_float_arg(const char *name, const char *value, float min_value, float max_value, float *out)
{
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value || *end != '\0' || parsed < min_value || parsed > max_value)
    {
        printf("%s must be a float in [%.3f, %.3f], got '%s'\n", name, min_value, max_value, value);
        return false;
    }
    *out = parsed;
    return true;
}

static bool parse_class_ids(const char *name, const char *value, std::vector<int> *out)
{
    out->clear();
    if (value == NULL || *value == '\0')
        return true;
    std::set<int> seen;
    std::istringstream input(value);
    std::string item;
    while (std::getline(input, item, ','))
    {
        int class_id = 0;
        if (!parse_int_arg(name, item.c_str(), 0, 100000, &class_id) ||
            !seen.insert(class_id).second)
        {
            printf("%s must contain unique non-negative integers\n", name);
            return false;
        }
        out->push_back(class_id);
    }
    return true;
}

static bool parse_intrusion_region(const char *value, IntrusionRegion *region)
{
    float values[4];
    std::istringstream input(value);
    std::string item;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::getline(input, item, ',') ||
            !parse_float_arg("--intrusion-region", item.c_str(), 0.0f, 1.0f, &values[i]))
        {
            return false;
        }
    }
    if (std::getline(input, item, ',') || !(values[0] < values[2] && values[1] < values[3]))
    {
        printf("--intrusion-region must be ordered x1,y1,x2,y2 in [0,1]\n");
        return false;
    }
    region->enabled = true;
    region->x1 = values[0];
    region->y1 = values[1];
    region->x2 = values[2];
    region->y2 = values[3];
    return true;
}

static bool parse_tracker_roi(const char *value, TrackingRoiConfig *roi)
{
    if (strcmp(value, "disabled") == 0)
    {
        *roi = TrackingRoiConfig();
        return true;
    }
    float values[4];
    std::istringstream input(value);
    std::string item;
    for (int i = 0; i < 4; ++i)
    {
        if (!std::getline(input, item, ',') || !parse_float_arg("--tracker-roi", item.c_str(), 0.0f, 1.0f, &values[i]))
            return false;
    }
    if (std::getline(input, item, ',') || !(values[0] < values[2] && values[1] < values[3]))
    {
        printf("--tracker-roi must be disabled or ordered x1,y1,x2,y2 in [0,1]\n");
        return false;
    }
    roi->enabled = true;
    roi->x1 = values[0]; roi->y1 = values[1]; roi->x2 = values[2]; roi->y2 = values[3];
    return true;
}

static const char *local_display_mode_to_string(LocalDisplayMode mode)
{
    switch (mode)
    {
    case LOCAL_DISPLAY_FAKESINK:
        return "fakesink";
    case LOCAL_DISPLAY_DISABLED:
    default:
        return "none";
    }
}

static bool parse_local_display_mode(const char *value, LocalDisplayMode *out)
{
    if (strcmp(value, "none") == 0 || strcmp(value, "disabled") == 0 || strcmp(value, "off") == 0)
    {
        *out = LOCAL_DISPLAY_DISABLED;
        return true;
    }
    if (strcmp(value, "fakesink") == 0 || strcmp(value, "placeholder") == 0)
    {
        *out = LOCAL_DISPLAY_FAKESINK;
        return true;
    }
    printf("--display-mode must be none or fakesink, got '%s'\n", value);
    return false;
}

static const char *stream_tracker_type_to_string(StreamTrackerType type)
{
    return type == STREAM_TRACKER_BYTETRACK ? "bytetrack" : "none";
}

static bool parse_stream_tracker_type(const char *value, StreamTrackerType *out)
{
    if (strcmp(value, "none") == 0 || strcmp(value, "disabled") == 0 || strcmp(value, "off") == 0)
    {
        *out = STREAM_TRACKER_NONE;
        return true;
    }
    if (strcmp(value, "bytetrack") == 0 || strcmp(value, "byte") == 0)
    {
        *out = STREAM_TRACKER_BYTETRACK;
        return true;
    }
    printf("--tracker-type must be none or bytetrack, got '%s'\n", value);
    return false;
}

static bool is_local_display_branch_enabled(const StreamServerConfig &config)
{
    return config.display_mode == LOCAL_DISPLAY_FAKESINK;
}

static bool is_ai_branch_enabled(const StreamServerConfig &config)
{
    return !config.model_path.empty();
}

static bool is_tracking_enabled(const StreamServerConfig &config)
{
    return config.tracker_type == STREAM_TRACKER_BYTETRACK &&
           !config.model_path.empty();
}

static bool is_intrusion_enabled(const StreamServerConfig &config)
{
    return config.intrusion_config.enabled;
}

static const char *stream_runtime_version()
{
    return "V4.0.0";
}

static void configure_mpp_rga_policy()
{
    if (getenv("GST_MPP_NO_RGA") == NULL)
    {
        setenv("GST_MPP_NO_RGA", "1", 0);
    }
}

static bool is_mpp_rga_disabled()
{
    const char *value = getenv("GST_MPP_NO_RGA");
    return value != NULL && strcmp(value, "0") != 0;
}

static const char *local_overlay_renderer_name()
{
    static NullLocalOverlayRenderer renderer;
    return renderer.name();
}

static long long now_epoch_ms()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static std::string normalize_rtsp_path(const std::string &path)
{
    if (path.empty())
    {
        return "/rknn_detect";
    }
    if (path[0] == '/')
    {
        return path;
    }
    return "/" + path;
}

static std::string make_rtsp_url(const StreamServerConfig &config)
{
    const char *advertised_host = getenv("RKNN_DETECT_RTSP_HOST");
    if (advertised_host == NULL || *advertised_host == '\0')
        advertised_host = "board.local";
    std::ostringstream oss;
    oss << "rtsp://" << advertised_host << ":" << config.rtsp_port << normalize_rtsp_path(config.rtsp_path);
    return oss.str();
}

static std::string gstreamer_rtsp_launch_string(const StreamServerConfig &config)
{
    std::ostringstream launch;
    launch << "( v4l2src name=src device=" << config.video_node
           << " do-timestamp=true "
           << "! video/x-raw,format=" << config.pixel_format
           << ",width=" << config.width
           << ",height=" << config.height
           << ",framerate=" << config.fps << "/1 "
           << "! tee name=t "
           << "t. ! queue name=rtsp_q leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 "
           << "! mpph264enc bps=" << config.h264_bitrate
           << " gop=" << config.rtsp_fps
           << " header-mode=each-idr "
           << "! h264parse name=rtsp_h264parse config-interval=1 "
           << "! rtph264pay name=pay0 pt=96 ";
    if (is_ai_branch_enabled(config))
    {
        launch << "t. ! queue name=ai_q leaky=upstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 "
               << "! videorate drop-only=true max-rate=" << config.inference_fps << " "
               << "! video/x-raw,format=NV12,width=" << config.width
               << ",height=" << config.height
               << ",framerate=" << config.inference_fps << "/1 "
               << "! appsink name=ai_sink emit-signals=true sync=false drop=true max-buffers=1 ";
    }
    if (is_local_display_branch_enabled(config))
    {
        launch << "t. ! queue name=display_q leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 "
               << "! fakesink name=display_sink sync=false async=false ";
    }
    launch << ")";
    return launch.str();
}

static std::string rtsp_launch_string(const StreamServerConfig &config)
{
    return gstreamer_rtsp_launch_string(config);
}

static std::string json_escape(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (size_t i = 0; i < value.size(); ++i)
    {
        char c = value[i];
        switch (c)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

static std::string make_status_json(const StreamServerConfig &config, RuntimeEvidence *stream_state)
{
    const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
    const long long now_ms = now_epoch_ms();
    const bool ai_branch_enabled = is_ai_branch_enabled(config);
    const long long rtsp_latest_video_age_ms =
        snapshot.latest_rtsp_push_time_ms > 0 ? now_ms - snapshot.latest_rtsp_push_time_ms : -1;
    long long alignment_frame_id = snapshot.latest_rtsp_capture_frame_id;
    long long alignment_frame_time_ms = snapshot.latest_rtsp_capture_frame_time_ms;
    const char *alignment_source = "rtsp-clean";
    StreamFrameMetadata alignment_frame;
    alignment_frame.capture_frame_id = alignment_frame_id;
    alignment_frame.capture_time_ms = alignment_frame_time_ms;
    std::vector<DetectionOverlayResult> detection_results(snapshot.detection_result_ring.begin(),
                                                          snapshot.detection_result_ring.end());
    OverlaySelection alignment = select_overlay_result_for_frame(alignment_frame, detection_results,
                                                                 config.latest_hold_ms);
    std::string alignment_state = alignment.state;
    int alignment_lag_frames = alignment.lag_frames;
    long long alignment_lag_ms = alignment.lag_ms;
    bool alignment_drawn = alignment.draw;
    size_t detection_result_ring_size = snapshot.detection_result_ring.size();
    long long overlay_detection_frame_id = alignment.detection_capture_frame_id;
    long long overlay_detection_capture_time_ms = alignment.detection_capture_time_ms;
    long long overlay_detection_update_time_ms = alignment.detection_update_time_ms;
    if (snapshot.latest_rtsp_capture_frame_id > 0 && !snapshot.latest_rtsp_alignment_state.empty())
    {
        alignment_state = snapshot.latest_rtsp_alignment_state;
        alignment_lag_frames = snapshot.latest_rtsp_overlay_lag_frames;
        alignment_lag_ms = snapshot.latest_rtsp_overlay_lag_ms;
        alignment_drawn = snapshot.latest_rtsp_overlay_drawn;
        overlay_detection_frame_id = snapshot.latest_rtsp_overlay_detection_frame_id;
        overlay_detection_capture_time_ms = snapshot.latest_rtsp_overlay_detection_capture_time_ms;
        overlay_detection_update_time_ms = snapshot.latest_rtsp_overlay_detection_update_time_ms;
    }

    char buf[12288];
    snprintf(buf, sizeof(buf),
             "{"
             "\"state\":\"%s\","
             "\"version\":\"%s\","
             "\"ai_branch_enabled\":%s,"
             "\"ai_branch_queue_policy\":\"%s\","
             "\"video_node\":\"%s\","
             "\"width\":%d,"
             "\"height\":%d,"
             "\"fps\":%d,"
             "\"inference_fps_target\":%d,"
             "\"rtsp_enabled\":%s,"
             "\"rtsp_ready\":%s,"
             "\"rtsp_url\":\"%s\","
             "\"rtsp_port\":%d,"
             "\"rtsp_path\":\"%s\","
             "\"rtsp_fps_target\":%d,"
             "\"rtsp_clean_video\":%s,"
             "\"rtsp_branch_queue_policy\":\"%s\","
             "\"display_mode\":\"%s\","
             "\"local_display_enabled\":%s,"
             "\"local_display_sink\":\"%s\","
             "\"local_display_queue_policy\":\"%s\","
             "\"local_overlay_renderer\":\"%s\","
             "\"local_overlay_policy\":\"%s\","
             "\"h264_encoder\":\"%s\","
             "\"h264_bitrate\":%d,"
             "\"rtsp_latest_video_age_ms\":%lld,"
             "\"rtsp_pipeline_launch\":\"%s\","
             "\"output_delay_ms\":%d,"
             "\"latest_hold_ms\":%d,"
             "\"frame_ring_capacity\":%d,"
             "\"frame_ring_size\":%zu,"
             "\"detection_result_ring_capacity\":%zu,"
             "\"detection_result_ring_size\":%zu,"
             "\"stale_threshold_ms\":%d,"
             "\"pixel_format\":\"%s\","
             "\"model_type\":\"%s\","
             "\"model_path\":\"%s\","
             "\"preprocess_mode\":\"%s\","
             "\"box_conf_threshold\":%.3f,"
             "\"nms_threshold\":%.3f,"
             "\"preprocess_scale_x\":%.6f,"
             "\"preprocess_scale_y\":%.6f,"
             "\"preprocess_pad_x\":%.3f,"
             "\"preprocess_pad_y\":%.3f,"
             "\"camera_opened\":%s,"
             "\"capture_running\":%s,"
             "\"inference_running\":%s,"
             "\"inference_ready\":%s,"
             "\"capture_fps\":%.3f,"
             "\"rtsp_fps\":%.3f,"
             "\"inference_fps\":%.3f,"
             "\"captured_frames\":%lld,"
             "\"rtsp_encoded_frames\":%lld,"
             "\"rtsp_push_errors\":%lld,"
             "\"rtsp_dropped_frames\":%lld,"
             "\"ai_appsink_frames\":%lld,"
             "\"ai_appsink_dropped_frames\":%lld,"
             "\"ai_appsink_errors\":%lld,"
             "\"frame_ring_dropped_frames\":%lld,"
             "\"inference_frames\":%lld,"
             "\"inference_skipped_frames\":%lld,"
             "\"dropped_frames\":%lld,"
             "\"detection_count\":%d,"
             "\"latest_capture_frame_id\":%d,"
             "\"latest_capture_frame_time_ms\":%lld,"
             "\"latest_rtsp_capture_frame_id\":%lld,"
             "\"latest_rtsp_capture_frame_time_ms\":%lld,"
             "\"latest_rtsp_push_time_ms\":%lld,"
             "\"latest_detection_frame_id\":%d,"
             "\"latest_detection_capture_frame_time_ms\":%lld,"
             "\"latest_detection_time_ms\":%lld,"
             "\"last_preprocess_ms\":%.3f,"
             "\"last_inference_ms\":%.3f,"
             "\"last_postprocess_ms\":%.3f,"
             "\"overlay_latency_ms\":%.3f,"
             "\"overlay_lag_frames\":%d,"
             "\"overlay_lag_ms\":%lld,"
             "\"overlay_detection_frame_id\":%lld,"
             "\"overlay_detection_capture_time_ms\":%lld,"
             "\"overlay_detection_update_time_ms\":%lld,"
             "\"alignment_state\":\"%s\","
             "\"alignment_source\":\"%s\","
             "\"alignment_frame_id\":%lld,"
             "\"alignment_frame_time_ms\":%lld,"
             "\"alignment_overlay_drawn\":%s,"
             "\"last_update_ms\":%lld,"
             "\"message\":\"%s\""
             "}",
             snapshot.state.c_str(),
             stream_runtime_version(),
             ai_branch_enabled ? "true" : "false",
             ai_branch_enabled ? "queue leaky=upstream max-size-buffers=1 max-size-time=0 max-size-bytes=0 videorate drop-only=true appsink drop=true max-buffers=1" : "",
             json_escape(config.video_node).c_str(),
             config.width,
             config.height,
             config.fps,
             config.inference_fps,
             snapshot.rtsp_running ? "true" : "false",
             snapshot.rtsp_ready ? "true" : "false",
             json_escape(make_rtsp_url(config)).c_str(),
             config.rtsp_port,
             json_escape(normalize_rtsp_path(config.rtsp_path)).c_str(),
             config.rtsp_fps,
             "true",
             "queue leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0",
             local_display_mode_to_string(config.display_mode),
             snapshot.local_display_running ? "true" : "false",
             json_escape(snapshot.local_display_sink).c_str(),
             snapshot.local_display_running ? "queue leaky=downstream max-size-buffers=1 max-size-time=0 max-size-bytes=0" : "",
             local_overlay_renderer_name(),
             "select_overlay_result_for_frame",
             "mpph264enc",
             config.h264_bitrate,
             rtsp_latest_video_age_ms,
             json_escape(rtsp_launch_string(config)).c_str(),
             config.output_delay_ms,
             config.latest_hold_ms,
             config.frame_ring_size,
             snapshot.frame_ring_size,
             kDetectionResultRingCapacity,
             detection_result_ring_size,
             config.stale_threshold_ms,
             json_escape(config.pixel_format).c_str(),
             kModelType,
             json_escape(config.model_path).c_str(),
             preprocess_mode_to_string(config.preprocess_mode),
             config.box_conf_threshold,
             config.nms_threshold,
             snapshot.latest_preprocess_transform.scale_x,
             snapshot.latest_preprocess_transform.scale_y,
             snapshot.latest_preprocess_transform.pad_x,
             snapshot.latest_preprocess_transform.pad_y,
             snapshot.camera_opened ? "true" : "false",
             snapshot.capture_running ? "true" : "false",
             snapshot.inference_running ? "true" : "false",
             snapshot.inference_ready ? "true" : "false",
             snapshot.capture_fps,
             snapshot.rtsp_fps,
             snapshot.inference_fps,
             snapshot.captured_frames,
             snapshot.rtsp_encoded_frames,
             snapshot.rtsp_push_errors,
             snapshot.rtsp_dropped_frames,
             snapshot.ai_appsink_frames,
             snapshot.ai_appsink_dropped_frames,
             snapshot.ai_appsink_errors,
             snapshot.frame_ring_dropped_frames,
             snapshot.inference_frames,
             snapshot.inference_skipped_frames,
             snapshot.dropped_frames,
             snapshot.detection_count,
             snapshot.latest_capture_frame_id,
             snapshot.latest_capture_frame_time_ms,
             snapshot.latest_rtsp_capture_frame_id,
             snapshot.latest_rtsp_capture_frame_time_ms,
             snapshot.latest_rtsp_push_time_ms,
             snapshot.latest_detection_frame_id,
             snapshot.latest_detection_capture_frame_time_ms,
             snapshot.latest_detection_time_ms,
             snapshot.last_preprocess_ms,
             snapshot.last_inference_ms,
             snapshot.last_postprocess_ms,
             snapshot.overlay_latency_ms,
             alignment_lag_frames,
             alignment_lag_ms,
             overlay_detection_frame_id,
             overlay_detection_capture_time_ms,
             overlay_detection_update_time_ms,
             alignment_state.c_str(),
             alignment_source,
             alignment_frame_id,
             alignment_frame_time_ms,
             alignment_drawn ? "true" : "false",
             snapshot.last_update_ms,
             json_escape(snapshot.message).c_str());
    std::string status(buf);
    if (!status.empty() && status[status.size() - 1] == '}')
    {
        status.resize(status.size() - 1);
    }
    std::ostringstream tracker_status;
    std::ostringstream allowed_classes;
    allowed_classes << "[";
    for (size_t i = 0; i < config.observation_policy_config.allowed_class_ids.size(); ++i)
        allowed_classes << (i == 0 ? "" : ",") << config.observation_policy_config.allowed_class_ids[i];
    allowed_classes << "]";
    std::ostringstream rejected_reasons;
    rejected_reasons << "{";
    size_t reason_index = 0;
    for (std::map<std::string, long long>::const_iterator it = snapshot.profile_rejected_by_reason.begin();
         it != snapshot.profile_rejected_by_reason.end(); ++it, ++reason_index)
        rejected_reasons << (reason_index == 0 ? "" : ",") << "\"" << json_escape(it->first) << "\":" << it->second;
    rejected_reasons << "}";
    tracker_status << ",\"mpp_rga_disabled\":" << (is_mpp_rga_disabled() ? "true" : "false")
                   << ",\"mpp_rga_policy\":\"" << (is_mpp_rga_disabled() ? "GST_MPP_NO_RGA=1" : "enabled-by-environment") << "\""
                   << ",\"tracker_enabled\":" << (is_tracking_enabled(config) ? "true" : "false")
                   << ",\"tracker_type\":\"" << stream_tracker_type_to_string(config.tracker_type) << "\""
                   << ",\"tracker_state\":\"" << json_escape(snapshot.tracker_state) << "\""
                   << ",\"tracker_error\":\"" << json_escape(snapshot.tracker_error) << "\""
                   << ",\"tracker_low_threshold\":" << config.tracker_config.low_threshold
                   << ",\"tracker_high_threshold\":" << config.tracker_config.high_threshold
                   << ",\"tracker_new_track_threshold\":" << config.tracker_config.new_track_threshold
                   << ",\"tracker_match_threshold\":" << config.tracker_config.match_threshold
                   << ",\"tracker_second_match_threshold\":" << config.tracker_config.second_match_threshold
                   << ",\"tracker_confirm_hits\":" << config.tracker_config.confirm_hits
                   << ",\"tracker_lost_timeout_ms\":" << config.tracker_config.lost_timeout_ms
                   << ",\"tracker_max_tracks\":" << config.tracker_config.max_tracks
                   << ",\"profile_id\":\"" << json_escape(config.profile_id) << "\""
                   << ",\"profile_hash\":\"" << json_escape(config.profile_hash) << "\""
                   << ",\"allowed_class_ids\":" << allowed_classes.str()
                   << ",\"tracker_min_width\":" << config.observation_policy_config.min_width
                   << ",\"tracker_min_height\":" << config.observation_policy_config.min_height
                   << ",\"tracker_min_area\":" << config.observation_policy_config.min_area
                   << ",\"tracker_edge_margin\":" << config.observation_policy_config.edge_margin
                   << ",\"tracker_roi\":{\"enabled\":" << (config.observation_policy_config.roi.enabled ? "true" : "false")
                   << ",\"x1\":" << config.observation_policy_config.roi.x1
                   << ",\"y1\":" << config.observation_policy_config.roi.y1
                   << ",\"x2\":" << config.observation_policy_config.roi.x2
                   << ",\"y2\":" << config.observation_policy_config.roi.y2 << "}"
                   << ",\"profile_admitted_last_frame\":" << snapshot.profile_admitted_last_frame
                   << ",\"profile_rejected_last_frame\":" << snapshot.profile_rejected_last_frame
                   << ",\"profile_admitted_total\":" << snapshot.profile_admitted_total
                   << ",\"profile_rejected_total\":" << snapshot.profile_rejected_total
                   << ",\"profile_rejected_by_reason\":" << rejected_reasons.str()
                   << ",\"last_policy_ms\":" << snapshot.last_policy_ms
                   << ",\"average_policy_ms\":" << snapshot.average_policy_ms
                   << ",\"max_policy_ms\":" << snapshot.max_policy_ms
                   << ",\"tracker_candidate_count\":" << snapshot.tracker_candidate_count
                   << ",\"display_detection_count\":" << snapshot.display_detection_count
                   << ",\"high_pass_detection_count\":" << snapshot.high_pass_detection_count
                   << ",\"low_pass_detection_count\":" << snapshot.low_pass_detection_count
                   << ",\"low_only_candidate_count\":" << snapshot.low_only_candidate_count
                   << ",\"low_pass_high_duplicate_count\":" << snapshot.low_pass_high_duplicate_count
                   << ",\"rejected_low_pass_high_score_count\":" << snapshot.rejected_low_pass_high_score_count
                   << ",\"suppressed_low_only_overlap_count\":" << snapshot.suppressed_low_only_overlap_count
                   << ",\"tracker_candidate_capacity_dropped_count\":" << snapshot.tracker_candidate_capacity_dropped_count
                   << ",\"track_count\":" << snapshot.track_count
                   << ",\"tracker_tentative\":" << snapshot.tracker_diagnostics.tentative_count
                   << ",\"tracker_confirmed\":" << snapshot.tracker_diagnostics.confirmed_count
                   << ",\"tracker_lost\":" << snapshot.tracker_diagnostics.lost_count
                   << ",\"tracker_update_count\":" << snapshot.tracker_diagnostics.update_count
                   << ",\"tracker_reset_count\":" << snapshot.tracker_diagnostics.reset_count
                   << ",\"tracker_error_count\":" << snapshot.tracker_diagnostics.error_count
                   << ",\"tracker_dropped_observations\":" << snapshot.tracker_diagnostics.dropped_observations
                   << ",\"last_tracker_ms\":" << snapshot.tracker_diagnostics.last_update_ms
                   << ",\"average_tracker_ms\":" << snapshot.tracker_diagnostics.average_update_ms
                   << ",\"max_tracker_ms\":" << snapshot.tracker_diagnostics.max_update_ms
                   << ",\"latest_track_frame_id\":" << snapshot.latest_track_frame_id
                   << ",\"latest_track_capture_time_ms\":" << snapshot.latest_track_capture_time_ms
                   << ",\"latest_track_update_time_ms\":" << snapshot.latest_track_update_time_ms
                   << ",\"intrusion_enabled\":" << (is_intrusion_enabled(config) ? "true" : "false")
                   << ",\"intrusion_state\":\"" << json_escape(snapshot.intrusion_state) << "\""
                   << ",\"intrusion_error\":\"" << json_escape(snapshot.intrusion_error) << "\""
                   << ",\"intrusion_schema_version\":" << config.intrusion_schema_version
                   << ",\"intrusion_camera_id\":\"" << json_escape(config.intrusion_camera_id) << "\""
                   << ",\"intrusion_rule_id\":\"" << json_escape(config.intrusion_config.rule_id) << "\""
                   << ",\"intrusion_class_ids\":[";
    for (size_t i = 0; i < config.intrusion_config.class_ids.size(); ++i)
        tracker_status << (i == 0 ? "" : ",") << config.intrusion_config.class_ids[i];
    tracker_status
                   << "]"
                   << ",\"intrusion_region\":{\"enabled\":"
                   << (config.intrusion_config.region.enabled ? "true" : "false")
                   << ",\"x1\":" << config.intrusion_config.region.x1
                   << ",\"y1\":" << config.intrusion_config.region.y1
                   << ",\"x2\":" << config.intrusion_config.region.x2
                   << ",\"y2\":" << config.intrusion_config.region.y2 << "}"
                   << ",\"intrusion_dwell_ms\":" << config.intrusion_config.dwell_ms
                   << ",\"intrusion_boundary_hysteresis_px\":" << config.intrusion_config.boundary_hysteresis_px
                   << ",\"intrusion_prediction_grace_ms\":" << config.intrusion_config.prediction_grace_ms
                   << ",\"recording_enabled\":" << (config.recording_enabled ? "true" : "false")
                   << ",\"recording_state\":\"" << recorder_state_to_string(snapshot.recorder_diagnostics.state) << "\""
                   << ",\"recording_session_id\":\"" << json_escape(snapshot.recorder_diagnostics.recording_session_id) << "\""
                   << ",\"recording_queue_size\":" << snapshot.recorder_diagnostics.queue_size
                   << ",\"recording_queue_capacity\":" << snapshot.recorder_diagnostics.queue_capacity
                   << ",\"recording_written_records\":" << snapshot.recorder_diagnostics.written_records
                   << ",\"recording_dropped_records\":" << snapshot.recorder_diagnostics.dropped_records
                   << ",\"recording_bytes_written\":" << snapshot.recorder_diagnostics.bytes_written
                   << ",\"recording_current_segment\":" << snapshot.recorder_diagnostics.current_segment
                   << ",\"recording_error\":\"" << json_escape(snapshot.recorder_diagnostics.error) << "\""
                   << ",\"last_recorder_enqueue_ms\":" << snapshot.recorder_diagnostics.last_enqueue_ms
                   << ",\"max_recorder_enqueue_ms\":" << snapshot.recorder_diagnostics.max_enqueue_ms
                   << "}";
    return status + tracker_status.str();
}

static std::string make_recording_json(RuntimeEvidence *stream_state)
{
    const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
    const RecorderDiagnostics &value = snapshot.recorder_diagnostics;
    std::ostringstream json;
    json << "{\"state\":\"" << recorder_state_to_string(value.state) << "\""
         << ",\"session_id\":\"" << json_escape(value.recording_session_id) << "\""
         << ",\"path\":\"" << json_escape(value.session_path) << "\""
         << ",\"written_records\":" << value.written_records
         << ",\"dropped_records\":" << value.dropped_records
         << ",\"dropped_images\":" << value.dropped_images
         << ",\"queue_size\":" << value.queue_size << ",\"queue_capacity\":" << value.queue_capacity
         << ",\"bytes_written\":" << value.bytes_written << ",\"current_segment\":" << value.current_segment
         << ",\"error\":\"" << json_escape(value.error) << "\"}";
    return json.str();
}

static std::string make_detection_json(const StreamServerConfig &config, const char *state,
                                       int frame_id, long long frame_time_ms,
                                       long long update_time_ms, int source_width, int source_height,
                                       const preprocess_transform_t *transform,
                                       const detect_result_group_t *group,
                                       const DetectionResultViews &views,
                                       const perf_stats_t *perf_stats)
{
    std::string objects = "[";
    int count = group ? group->count : 0;
    for (int i = 0; i < count; ++i)
    {
        const detect_result_t *result = &group->results[i];
        char item[512];
        snprintf(item, sizeof(item),
                 "%s{"
                 "\"class_id\":%d,"
                 "\"class\":\"%s\","
                 "\"score\":%.6f,"
                 "\"x1\":%d,"
                 "\"y1\":%d,"
                 "\"x2\":%d,"
                 "\"y2\":%d"
                 "}",
                 i == 0 ? "" : ",",
                 result->class_id,
                 json_escape(result->name).c_str(),
                 result->prop,
                 result->box.left,
                 result->box.top,
                 result->box.right,
                 result->box.bottom);
        objects += item;
    }
    objects += "]";

    char header[4096];
    snprintf(header, sizeof(header),
             "{"
             "\"state\":\"%s\","
             "\"schema_version\":\"%s\","
             "\"result_type\":\"detection\","
             "\"tracking_state\":\"%s\","
             "\"model_type\":\"%s\","
             "\"capture_frame_id\":%d,"
             "\"capture_timestamp_ms\":%lld,"
             "\"update_time_ms\":%lld,"
             "\"latency_ms\":%lld,"
             "\"source_width\":%d,"
             "\"source_height\":%d,"
             "\"preprocess_mode\":\"%s\","
             "\"preprocess_scale_x\":%.6f,"
             "\"preprocess_scale_y\":%.6f,"
             "\"preprocess_pad_x\":%.3f,"
             "\"preprocess_pad_y\":%.3f,"
             "\"detection_count\":%d,"
             "\"postprocess_candidate_count\":%d,"
             "\"high_pass_detection_count\":%d,"
             "\"low_pass_detection_count\":%d,"
             "\"low_only_candidate_count\":%d,"
             "\"low_pass_high_duplicate_count\":%d,"
             "\"rejected_low_pass_high_score_count\":%d,"
             "\"suppressed_low_only_overlap_count\":%d,"
             "\"tracker_candidate_capacity_dropped_count\":%d,"
             "\"tracker_candidate_count\":%d,"
             "\"low_threshold\":%.3f,"
             "\"high_threshold\":%.3f,"
             "\"preprocess_ms\":%.3f,"
             "\"inference_ms\":%.3f,"
             "\"postprocess_ms\":%.3f,"
             "\"total_ms\":%.3f,"
             "\"objects\":",
             state,
             kStreamSchemaVersion,
             is_tracking_enabled(config) ? "separate" : "pass_through",
             kModelType,
             frame_id,
             frame_time_ms,
             update_time_ms,
             frame_time_ms > 0 ? update_time_ms - frame_time_ms : 0,
             source_width,
             source_height,
             preprocess_mode_to_string(transform ? transform->mode : PREPROCESS_MODE_RESIZE),
             transform ? transform->scale_x : 0.0f,
             transform ? transform->scale_y : 0.0f,
             transform ? transform->pad_x : 0.0f,
             transform ? transform->pad_y : 0.0f,
             count,
             views.candidate_count,
             views.high_pass_count,
             views.low_pass_count,
             views.low_only_count,
             views.low_pass_high_duplicate_count,
             views.rejected_low_pass_high_score_count,
             views.suppressed_low_only_overlap_count,
             views.capacity_dropped_count,
             views.tracker_candidates.count,
             is_tracking_enabled(config) ? config.tracker_config.low_threshold : config.box_conf_threshold,
             is_tracking_enabled(config) ? config.tracker_config.high_threshold : config.box_conf_threshold,
             perf_stats ? perf_stats->preprocess_ms : 0.0,
             perf_stats ? perf_stats->inference_ms : 0.0,
             perf_stats ? perf_stats->postprocess_ms : 0.0,
             perf_stats ? perf_stats->total_ms : 0.0);
    return std::string(header) + objects + "}";
}

static DetectionFrame make_tracker_detection_frame(int frame_id,
                                                    long long frame_time_ms,
                                                    int source_width,
                                                    int source_height,
                                                    const detect_result_group_t &group)
{
    DetectionFrame frame;
    frame.capture_frame_id = frame_id;
    frame.capture_timestamp_ms = frame_time_ms;
    frame.source_width = source_width;
    frame.source_height = source_height;
    for (int i = 0; i < group.count; ++i)
    {
        const detect_result_t &result = group.results[i];
        DetectionObject object;
        object.class_id = result.class_id;
        object.class_name = result.name;
        object.score = result.prop;
        object.bbox.x1 = static_cast<float>(result.box.left);
        object.bbox.y1 = static_cast<float>(result.box.top);
        object.bbox.x2 = static_cast<float>(result.box.right);
        object.bbox.y2 = static_cast<float>(result.box.bottom);
        frame.objects.push_back(object);
    }
    return frame;
}

static std::vector<AnalysisObservation> make_analysis_observations(const DetectionResultViews &views)
{
    std::vector<AnalysisObservation> observations;
    for (int i = 0; i < views.tracker_candidates.count; ++i)
    {
        const detect_result_t &source = views.tracker_candidates.results[i];
        AnalysisObservation observation;
        observation.observation_id = static_cast<uint32_t>(i + 1);
        observation.origin = source.prop >= views.high_threshold ? ObservationOrigin::HighPass : ObservationOrigin::LowOnly;
        observation.class_id = source.class_id;
        observation.class_name = source.name;
        observation.score = source.prop;
        observation.bbox.x1 = source.box.left;
        observation.bbox.y1 = source.box.top;
        observation.bbox.x2 = source.box.right;
        observation.bbox.y2 = source.box.bottom;
        observations.push_back(observation);
    }
    return observations;
}

static DetectionFrame make_policy_tracker_frame(int frame_id, long long frame_time_ms,
                                                int source_width, int source_height,
                                                const ObservationPolicyResult &policy)
{
    DetectionFrame frame;
    frame.capture_frame_id = frame_id;
    frame.capture_timestamp_ms = frame_time_ms;
    frame.source_width = source_width;
    frame.source_height = source_height;
    std::map<uint32_t, const AnalysisObservation *> by_id;
    for (size_t i = 0; i < policy.observations.size(); ++i)
        by_id[policy.observations[i].observation_id] = &policy.observations[i];
    for (size_t i = 0; i < policy.admitted_observation_ids.size(); ++i)
    {
        const AnalysisObservation &source = *by_id[policy.admitted_observation_ids[i]];
        DetectionObject object;
        object.class_id = source.class_id;
        object.class_name = source.class_name;
        object.score = source.score;
        object.bbox = source.bbox;
        frame.objects.push_back(object);
    }
    return frame;
}

static std::shared_ptr<RecordedFramePayload> make_recorded_frame_payload(const cv::Mat &frame)
{
    if (frame.empty() || frame.type() != CV_8UC3) return std::shared_ptr<RecordedFramePayload>();
    std::shared_ptr<RecordedFramePayload> payload(new RecordedFramePayload());
    payload->width = frame.cols; payload->height = frame.rows; payload->channels = frame.channels();
    const size_t bytes = static_cast<size_t>(frame.cols) * static_cast<size_t>(frame.rows) * static_cast<size_t>(frame.channels());
    payload->bgr.resize(bytes);
    if (frame.isContinuous()) memcpy(payload->bgr.data(), frame.data, bytes);
    else for (int row = 0; row < frame.rows; ++row) memcpy(payload->bgr.data() + static_cast<size_t>(row) * frame.cols * frame.channels(), frame.ptr(row), static_cast<size_t>(frame.cols) * frame.channels());
    return payload;
}

static std::string make_track_json(const StreamServerConfig &config,
                                   const char *state,
                                   const TrackFrame *frame,
                                   const TrackerDiagnostics &diagnostics,
                                   long long update_time_ms,
                                   const std::string &message)
{
    std::ostringstream json;
    json << "{\"state\":\"" << state << "\""
         << ",\"schema_version\":\"v4.0.0\""
         << ",\"result_type\":\"tracking\""
         << ",\"tracker_type\":\"" << stream_tracker_type_to_string(config.tracker_type) << "\""
         << ",\"capture_frame_id\":" << (frame ? frame->capture_frame_id : 0)
         << ",\"capture_timestamp_ms\":" << (frame ? frame->capture_timestamp_ms : 0)
         << ",\"update_time_ms\":" << update_time_ms
         << ",\"source_width\":" << (frame ? frame->source_width : 0)
         << ",\"source_height\":" << (frame ? frame->source_height : 0)
         << ",\"track_count\":" << (frame ? frame->objects.size() : 0)
         << ",\"tentative_count\":" << diagnostics.tentative_count
         << ",\"confirmed_count\":" << diagnostics.confirmed_count
         << ",\"lost_count\":" << diagnostics.lost_count
         << ",\"last_tracker_ms\":" << diagnostics.last_update_ms
         << ",\"message\":\"" << json_escape(message) << "\""
         << ",\"objects\":[";
    if (frame != NULL)
    {
        for (size_t i = 0; i < frame->objects.size(); ++i)
        {
            const TrackObject &object = frame->objects[i];
            json << (i == 0 ? "" : ",")
                 << "{\"track_id\":" << object.track_id
                 << ",\"class_id\":" << object.class_id
                 << ",\"class\":\"" << json_escape(object.class_name) << "\""
                 << ",\"track_state\":\"" << track_lifecycle_to_string(object.state) << "\""
                 << ",\"bbox_source\":\"" << tracker_bbox_source_to_string(object.bbox_source) << "\""
                 << ",\"score\":" << object.score
                 << ",\"x1\":" << object.bbox.x1
                 << ",\"y1\":" << object.bbox.y1
                 << ",\"x2\":" << object.bbox.x2
                 << ",\"y2\":" << object.bbox.y2
                 << ",\"track_age\":" << object.track_age
                 << ",\"hit_count\":" << object.hit_count
                 << ",\"missed_updates\":" << object.missed_updates
                 << "}";
        }
    }
    json << "]}";
    return json.str();
}

static void append_intrusion_target_json(std::ostringstream *json, const IntrusionTarget &target)
{
    *json << "{\"track_id\":" << target.track_id
          << ",\"class_id\":" << target.class_id
          << ",\"class_name\":\"" << json_escape(target.class_name) << "\""
          << ",\"bbox_source\":\"" << tracker_bbox_source_to_string(target.bbox_source) << "\""
          << ",\"bbox\":[" << target.bbox.x1 << "," << target.bbox.y1 << ","
          << target.bbox.x2 << "," << target.bbox.y2 << "]"
          << ",\"anchor_point\":[" << target.anchor_x << "," << target.anchor_y << "]"
          << ",\"state\":\"" << intrusion_target_state_to_string(target.state) << "\""
          << ",\"dwell_ms\":" << target.dwell_ms
          << ",\"threshold_ms\":" << target.threshold_ms
          << ",\"capture_frame_id\":" << target.capture_frame_id
          << ",\"capture_timestamp_ms\":" << target.capture_timestamp_ms
          << ",\"event_sequence\":" << target.event_sequence
          << "}";
}

static std::string make_intrusion_json(const StreamServerConfig &config, const char *state,
                                       const IntrusionFrame *frame, long long update_time_ms,
                                       const std::string &message)
{
    IntrusionFrame empty;
    const IntrusionFrame &value = frame != NULL ? *frame : empty;
    std::ostringstream json;
    json << "{\"schema_version\":" << config.intrusion_schema_version
         << ",\"runtime_session_id\":\"" << json_escape(config.recording_config.runtime_session_id) << "\""
         << ",\"camera_id\":\"" << json_escape(config.intrusion_camera_id) << "\""
         << ",\"state\":\"" << state << "\""
         << ",\"enabled\":" << (is_intrusion_enabled(config) ? "true" : "false")
         << ",\"rule_id\":\"" << json_escape(config.intrusion_config.rule_id) << "\""
         << ",\"class_ids\":[";
    for (size_t i = 0; i < config.intrusion_config.class_ids.size(); ++i)
        json << (i == 0 ? "" : ",") << config.intrusion_config.class_ids[i];
    json
         << "]"
         << ",\"region\":{\"type\":\"rectangle\",\"x1\":" << config.intrusion_config.region.x1
         << ",\"y1\":" << config.intrusion_config.region.y1
         << ",\"x2\":" << config.intrusion_config.region.x2
         << ",\"y2\":" << config.intrusion_config.region.y2 << "}"
         << ",\"dwell_ms\":" << config.intrusion_config.dwell_ms
         << ",\"boundary_hysteresis_px\":" << config.intrusion_config.boundary_hysteresis_px
         << ",\"prediction_grace_ms\":" << config.intrusion_config.prediction_grace_ms
         << ",\"capture_frame_id\":" << value.capture_frame_id
         << ",\"capture_timestamp_ms\":" << value.capture_timestamp_ms
         << ",\"source_width\":" << value.source_width
         << ",\"source_height\":" << value.source_height
         << ",\"event_sequence\":" << value.event_sequence
         << ",\"update_time_ms\":" << update_time_ms
         << ",\"in_region_targets\":[";
    for (size_t i = 0; i < value.in_region_targets.size(); ++i)
    {
        if (i != 0) json << ",";
        append_intrusion_target_json(&json, value.in_region_targets[i]);
    }
    json << "],\"active_alarms\":[";
    for (size_t i = 0; i < value.active_alarms.size(); ++i)
    {
        if (i != 0) json << ",";
        append_intrusion_target_json(&json, value.active_alarms[i]);
    }
    json << "],\"recent_events\":[";
    for (size_t i = 0; i < value.recent_events.size(); ++i)
    {
        if (i != 0) json << ",";
        json << "{\"event_sequence\":" << value.recent_events[i].event_sequence
             << ",\"event_type\":\"" << json_escape(value.recent_events[i].event_type) << "\",\"target\":";
        append_intrusion_target_json(&json, value.recent_events[i].target);
        json << "}";
    }
    json << "],\"message\":\"" << json_escape(message) << "\"}";
    return json.str();
}

static bool send_all(int fd, const unsigned char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size)
    {
        ssize_t n = send(fd, data + sent, size - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool send_all(int fd, const std::string &data)
{
    return send_all(fd, (const unsigned char *)data.data(), data.size());
}

static void send_response(int client_fd, int status_code, const char *status_text,
                          const char *content_type, const std::string &body)
{
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             status_code, status_text, content_type, body.size());
    send_all(client_fd, std::string(header) + body);
}

static void send_binary_response(int client_fd, const char *content_type, const std::vector<unsigned char> &body)
{
    char header[512];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "\r\n",
             content_type, body.size());
    send_all(client_fd, std::string(header));
    send_all(client_fd, body.data(), body.size());
}

static std::string request_path(const std::string &request)
{
    size_t first_space = request.find(' ');
    if (first_space == std::string::npos)
    {
        return "/";
    }
    size_t second_space = request.find(' ', first_space + 1);
    if (second_space == std::string::npos)
    {
        return "/";
    }
    return request.substr(first_space + 1, second_space - first_space - 1);
}

static void update_message(RuntimeEvidence *stream_state, const std::string &state, const std::string &message)
{
    stream_state->set_message(state, message, now_epoch_ms());
}

static void initialize_gstreamer_state(const StreamServerConfig &config, RuntimeEvidence *stream_state)
{
    long long update_ms = now_epoch_ms();
    const std::string detection_json =
        std::string("{\"state\":\"disabled\",\"model_type\":\"") +
        kModelType + "\",\"schema_version\":\"" + kStreamSchemaVersion + "\",\"result_type\":\"detection\","
        "\"tracking_state\":\"pass_through\","
        "\"capture_frame_id\":0,\"capture_timestamp_ms\":0,"
        "\"update_time_ms\":" +
        std::to_string(update_ms) +
        ",\"latency_ms\":0,\"source_width\":0,\"source_height\":0,"
        "\"detection_count\":0,\"message\":\"AI appsink branch waiting for a model\","
        "\"objects\":[]}";
    const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
    const std::string tracker_state = is_tracking_enabled(config) ? "starting" : "disabled";
    const std::string track_json = make_track_json(
        config, tracker_state.c_str(), NULL, snapshot.tracker_diagnostics, update_ms,
        is_tracking_enabled(config) ? "tracker waiting for AI frames" : "tracking disabled");
    stream_state->initialize_gstreamer_state(
        is_local_display_branch_enabled(config),
        is_local_display_branch_enabled(config) ? "fakesink" : "none",
        std::string(stream_runtime_version()) + " GStreamer clean RTSP pipeline starting",
        detection_json, tracker_state, "", track_json, update_ms);
}

static bool make_snapshot_jpeg(RuntimeEvidence *stream_state, std::vector<unsigned char> *jpeg, int *frame_id)
{
    cv::Mat frame;
    int capture_frame_id = 0;
    if (!stream_state->copy_latest_frame(&frame, &capture_frame_id)) return false;

    std::vector<int> encode_params;
    encode_params.push_back(cv::IMWRITE_JPEG_QUALITY);
    encode_params.push_back(kSnapshotJpegQuality);
    if (!cv::imencode(".jpg", frame, *jpeg, encode_params))
    {
        return false;
    }
    *frame_id = capture_frame_id;
    return true;
}

static std::string get_latest_detection_json(RuntimeEvidence *stream_state)
{
    return stream_state->latest_detection_json();
}

static std::string get_latest_track_json(RuntimeEvidence *stream_state)
{
    return stream_state->latest_track_json();
}

static std::string get_latest_intrusion_json(RuntimeEvidence *stream_state)
{
    return stream_state->latest_intrusion_json();
}

static void initialize_intrusion_state(const StreamServerConfig &config, RuntimeEvidence *stream_state)
{
    const long long update_ms = now_epoch_ms();
    const char *state = is_intrusion_enabled(config) ? "starting" : "disabled";
    stream_state->initialize_intrusion_state(
        state, "", make_intrusion_json(config, state, NULL, update_ms,
                                        is_intrusion_enabled(config)
                                            ? "intrusion evaluator starting"
                                            : "intrusion disabled"), update_ms);
}

static void update_detection_error(RuntimeEvidence *stream_state, const StreamServerConfig &config,
                                   const std::string &message)
{
    long long update_ms = now_epoch_ms();
    const std::string detection_json =
        "{\"state\":\"error\",\"model_type\":\"" +
        std::string(kModelType) +
        "\",\"schema_version\":\"" + std::string(kStreamSchemaVersion) +
        "\",\"result_type\":\"detection\",\"tracking_state\":\"" +
        (is_tracking_enabled(config) ? "error" : "pass_through") + "\","
        "\"capture_frame_id\":0,\"capture_timestamp_ms\":0,\"update_time_ms\":" +
        std::to_string(update_ms) +
        ",\"latency_ms\":0,\"source_width\":0,\"source_height\":0,\"detection_count\":0,\"message\":\"" +
        json_escape(message) + "\",\"objects\":[]}";
    const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
    const std::string tracker_state = is_tracking_enabled(config) ? "error" : "disabled";
    const std::string intrusion_state = is_intrusion_enabled(config) ? "unavailable" : "disabled";
    stream_state->set_inference_status(
        false, false, detection_json, tracker_state,
        is_tracking_enabled(config) ? message : "",
        make_track_json(config, tracker_state.c_str(), NULL, snapshot.tracker_diagnostics, update_ms, message),
        intrusion_state, is_intrusion_enabled(config) ? message : "",
        make_intrusion_json(config, intrusion_state.c_str(), NULL, update_ms,
                            is_intrusion_enabled(config) ? message : "intrusion disabled"),
        update_ms, "error", message);
}

static bool get_latest_inference_frame(RuntimeEvidence *stream_state, int last_processed_frame_id,
                                       cv::Mat *frame, int *frame_id, long long *frame_time_ms)
{
    return stream_state->wait_for_inference_frame(last_processed_frame_id, frame, frame_id,
                                                  frame_time_ms, g_stop_requested != 0);
}

static void inference_loop(const StreamServerConfig config, RuntimeEvidence *stream_state, IAnalysisRecorder *recorder)
{
    if (config.model_path.empty())
    {
        long long update_ms = now_epoch_ms();
        const std::string detection_json =
            "{\"state\":\"disabled\",\"model_type\":\"" +
            std::string(kModelType) +
            "\",\"schema_version\":\"" + std::string(kStreamSchemaVersion) +
            "\",\"result_type\":\"detection\",\"tracking_state\":\"pass_through\","
            "\"capture_frame_id\":0,\"capture_timestamp_ms\":0,\"update_time_ms\":" +
            std::to_string(update_ms) +
            ",\"latency_ms\":0,\"source_width\":0,\"source_height\":0,\"detection_count\":0,\"message\":\"no RKNN model path\",\"objects\":[]}";
        const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
        const std::string intrusion_state = is_intrusion_enabled(config) ? "unavailable" : "disabled";
        stream_state->set_inference_status(
            false, false, detection_json, "disabled", "",
            make_track_json(config, "disabled", NULL, snapshot.tracker_diagnostics,
                            update_ms, "no RKNN model path"),
            intrusion_state, is_intrusion_enabled(config) ? "no RKNN model path" : "",
            make_intrusion_json(config, intrusion_state.c_str(), NULL, update_ms,
                                is_intrusion_enabled(config) ? "no RKNN model path" : "intrusion disabled"),
            update_ms);
        return;
    }

    set_detector_inference_logging(false);
    set_opencv_preprocess_logging(false);
    set_postprocess_candidate_logging(false);

    rknn_context ctx = 0;
    unsigned char *model_data = NULL;
    int model_data_size = 0;
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    std::vector<rknn_tensor_attr> input_attrs;
    std::vector<rknn_tensor_attr> output_attrs;
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    int model_height = 0;
    int model_width = 0;
    int model_channel = 0;

    model_data = load_model(config.model_path.c_str(), &model_data_size);
    if (model_data == NULL)
    {
        update_detection_error(stream_state, config, "failed to load model");
        return;
    }

    int ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        free(model_data);
        update_detection_error(stream_state, config, "rknn_init failed");
        return;
    }

    if (query_sdk_version(ctx) < 0 || query_io_num(ctx, &io_num) < 0)
    {
        release_runtime(ctx, model_data, NULL);
        update_detection_error(stream_state, config, "failed to query RKNN runtime");
        return;
    }

    input_attrs.resize(io_num.n_input);
    memset(input_attrs.data(), 0, sizeof(rknn_tensor_attr) * input_attrs.size());
    output_attrs.resize(io_num.n_output);
    memset(output_attrs.data(), 0, sizeof(rknn_tensor_attr) * output_attrs.size());
    if (query_input_attrs(ctx, input_attrs.data(), io_num.n_input) < 0 ||
        query_output_attrs(ctx, output_attrs.data(), io_num.n_output) < 0)
    {
        release_runtime(ctx, model_data, NULL);
        update_detection_error(stream_state, config, "failed to query RKNN tensor attributes");
        return;
    }

    get_model_input_shape(&input_attrs[0], &model_height, &model_width, &model_channel);
    if (model_height <= 0 || model_width <= 0 || model_channel != 3)
    {
        release_runtime(ctx, model_data, NULL);
        update_detection_error(stream_state, config, "unsupported RKNN model input shape");
        return;
    }
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    std::vector<unsigned char> input_buf(model_width * model_height * model_channel);
    std::string tracker_init_error;
    std::unique_ptr<IObjectTracker> object_tracker;
    bool tracker_operational = false;
    TrackerDiagnostics last_tracker_diagnostics;
    ObservationPolicy observation_policy(config.observation_policy_config, config.tracker_config.low_threshold);
    if (is_tracking_enabled(config))
    {
        object_tracker = create_bytetrack_object_tracker(config.tracker_config, &tracker_init_error);
        tracker_operational = object_tracker.get() != NULL;
    }
    std::unique_ptr<IntrusionEvaluator> intrusion_evaluator;
    bool intrusion_operational = false;
    std::string intrusion_init_error;
    if (is_intrusion_enabled(config))
    {
        if (tracker_operational)
        {
            intrusion_evaluator.reset(new IntrusionEvaluator(config.intrusion_config));
            std::string intrusion_validation_error;
            if (!validate_intrusion_rule_config(config.intrusion_config, &intrusion_validation_error))
            {
                intrusion_init_error = intrusion_validation_error;
            }
            else
            {
                intrusion_operational = true;
            }
        }
        else
        {
            intrusion_init_error = tracker_init_error.empty() ? "tracker unavailable" : tracker_init_error;
        }
    }
    {
        long long update_ms = now_epoch_ms();
        const std::string detection_json =
            "{\"state\":\"running\",\"model_type\":\"" +
            std::string(kModelType) +
            "\",\"schema_version\":\"" + std::string(kStreamSchemaVersion) +
            "\",\"result_type\":\"detection\",\"tracking_state\":\"" +
            (is_tracking_enabled(config) ? "separate" : "pass_through") + "\","
            "\"capture_frame_id\":0,\"capture_timestamp_ms\":0,\"update_time_ms\":" +
            std::to_string(update_ms) +
            ",\"latency_ms\":0,\"source_width\":0,\"source_height\":0,\"detection_count\":0,\"objects\":[]}";
        const std::string tracker_state = !is_tracking_enabled(config) ? "disabled"
                                      : tracker_operational ? "running" : "error";
        const std::string intrusion_state = !is_intrusion_enabled(config) ? "disabled"
                                      : intrusion_operational ? "running" : "unavailable";
        const std::string intrusion_json = make_intrusion_json(
            config, intrusion_state.c_str(), NULL, update_ms,
            intrusion_operational ? "intrusion evaluator ready"
                                  : (is_intrusion_enabled(config) ? intrusion_init_error
                                                                   : "intrusion disabled"));
        const RuntimeEvidenceSnapshot snapshot = stream_state->snapshot();
        stream_state->set_inference_status(
            true, true, detection_json, tracker_state, tracker_init_error,
            make_track_json(config, tracker_state.c_str(), NULL, snapshot.tracker_diagnostics,
                            update_ms, tracker_operational ? "tracker ready" : tracker_init_error),
            intrusion_state, intrusion_init_error, intrusion_json, update_ms);
    }

    int last_processed_frame_id = 0;
    const long long min_interval_ms = config.inference_fps > 0 ? 1000 / config.inference_fps : 0;
    long long last_inference_start_ms = 0;

    while (!g_stop_requested)
    {
        long long now_ms = now_epoch_ms();
        if (last_inference_start_ms > 0 && min_interval_ms > 0 &&
            now_ms - last_inference_start_ms < min_interval_ms)
        {
            usleep((useconds_t)((min_interval_ms - (now_ms - last_inference_start_ms)) * 1000));
        }

        cv::Mat frame;
        int frame_id = 0;
        long long frame_time_ms = 0;
        if (!get_latest_inference_frame(stream_state, last_processed_frame_id, &frame, &frame_id, &frame_time_ms))
        {
            continue;
        }
        last_processed_frame_id = frame_id;
        last_inference_start_ms = now_epoch_ms();

        if (config.pixel_format == "NV12" &&
            frame.type() == CV_8UC1 &&
            frame.cols == config.width &&
            frame.rows == config.height + config.height / 2)
        {
            cv::Mat bgr_frame;
            try
            {
                cv::cvtColor(frame, bgr_frame, cv::COLOR_YUV2BGR_NV12);
            }
            catch (const cv::Exception &)
            {
                update_detection_error(stream_state, config, "failed to convert appsink NV12 frame");
                break;
            }
            frame = bgr_frame;
        }

        preprocess_transform_t preprocess_transform;
        memset(&preprocess_transform, 0, sizeof(preprocess_transform));
        double preprocess_ms = 0.0;
        ret = preprocess_frame_opencv(frame, model_width, model_height, model_channel,
                                      input_buf.data(), config.preprocess_mode, &preprocess_transform,
                                      false, "", &preprocess_ms);
        if (ret < 0)
        {
            update_detection_error(stream_state, config, "failed to preprocess camera frame");
            break;
        }

        perf_stats_t perf_stats;
        memset(&perf_stats, 0, sizeof(perf_stats));
        perf_stats.preprocess_ms = preprocess_ms;

        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = model_width * model_height * model_channel;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].pass_through = 0;
        inputs[0].buf = input_buf.data();

        std::vector<rknn_output> outputs(io_num.n_output);
        memset(outputs.data(), 0, sizeof(rknn_output) * outputs.size());
        for (int i = 0; i < io_num.n_output; ++i)
        {
            outputs[i].want_float = 0;
        }

        ret = run_rknn_inference(ctx, io_num, inputs, outputs.data(), &perf_stats);
        if (ret < 0)
        {
            update_detection_error(stream_state, config, "RKNN inference failed");
            break;
        }

        DetectionResultViews detection_views;
        detect_result_group_t high_pass_results;
        detect_result_group_t low_pass_results;
        memset(&high_pass_results, 0, sizeof(high_pass_results));
        memset(&low_pass_results, 0, sizeof(low_pass_results));

        perf_stats_t high_pass_perf;
        perf_stats_t low_pass_perf;
        memset(&high_pass_perf, 0, sizeof(high_pass_perf));
        memset(&low_pass_perf, 0, sizeof(low_pass_perf));

        ret = run_yolov6_postprocess(outputs.data(), output_attrs.data(), io_num.n_output,
                                     model_height, model_width, config.box_conf_threshold,
                                     config.nms_threshold, &preprocess_transform, out_zps, out_scales,
                                     &high_pass_results, &high_pass_perf);
        if (ret >= 0 && is_tracking_enabled(config))
        {
            ret = run_yolov6_postprocess(outputs.data(), output_attrs.data(), io_num.n_output,
                                         model_height, model_width, config.tracker_config.low_threshold,
                                         config.nms_threshold, &preprocess_transform, out_zps, out_scales,
                                         &low_pass_results, &low_pass_perf);
        }
        rknn_outputs_release(ctx, io_num.n_output, outputs.data());
        perf_stats.postprocess_ms = high_pass_perf.postprocess_ms + low_pass_perf.postprocess_ms;
        if (ret < 0)
        {
            std::string message = std::string(kModelType) + " postprocess failed";
            update_detection_error(stream_state, config, message);
            break;
        }

        if (is_tracking_enabled(config))
        {
            ret = build_dual_pass_detection_result_views(&high_pass_results,
                                                         &low_pass_results,
                                                         config.tracker_config.low_threshold,
                                                         config.tracker_config.high_threshold,
                                                         config.nms_threshold,
                                                         &detection_views);
            if (ret < 0)
            {
                update_detection_error(stream_state, config, "failed to build dual-pass detection threshold views");
                break;
            }
        }
        else
        {
            detection_views.low_threshold = config.box_conf_threshold;
            detection_views.high_threshold = config.box_conf_threshold;
            detection_views.candidate_count = high_pass_results.count;
            detection_views.high_pass_count = high_pass_results.count;
            detection_views.low_pass_count = high_pass_results.count;
            detection_views.display_detections = high_pass_results;
        }

        update_perf_totals(&perf_stats);
        long long update_ms = now_epoch_ms();
        TrackFrame track_frame;
        ObservationPolicyResult policy_result;
        double policy_ms = 0.0;
        TrackerDiagnostics tracker_diagnostics = last_tracker_diagnostics;
        std::string tracker_state = "disabled";
        std::string tracker_message = "tracking disabled";
        if (is_tracking_enabled(config))
        {
            tracker_state = tracker_operational ? "running" : "error";
            tracker_message = tracker_operational ? "tracker running" : tracker_init_error;
            if (tracker_operational)
            {
                const std::chrono::steady_clock::time_point policy_begin = std::chrono::steady_clock::now();
                std::string policy_error;
                const bool policy_ok = observation_policy.apply(make_analysis_observations(detection_views),
                                                                frame.cols, frame.rows, &policy_result, &policy_error);
                policy_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - policy_begin).count();
                if (!policy_ok)
                {
                    tracker_operational = false;
                    tracker_state = "error";
                    tracker_message = policy_error;
                }
                const DetectionFrame tracker_input = make_policy_tracker_frame(
                    frame_id, frame_time_ms, frame.cols, frame.rows, policy_result);
                std::string tracker_update_error;
                if (policy_ok && !object_tracker->update(tracker_input, &track_frame, &tracker_update_error))
                {
                    tracker_operational = false;
                    tracker_state = "error";
                    tracker_message = tracker_update_error;
                }
                tracker_diagnostics = object_tracker->diagnostics();
                last_tracker_diagnostics = tracker_diagnostics;
            }
        }
        const std::string track_json = make_track_json(config,
                                                       tracker_state.c_str(),
                                                       tracker_operational ? &track_frame : NULL,
                                                       tracker_diagnostics,
                                                       update_ms,
                                                       tracker_message);
        IntrusionFrame intrusion_frame;
        std::string intrusion_update_error;
        bool intrusion_frame_valid = false;
        if (is_intrusion_enabled(config) && intrusion_operational && tracker_operational)
        {
            intrusion_frame_valid = intrusion_evaluator->update(track_frame, &intrusion_frame,
                                                                &intrusion_update_error);
            if (!intrusion_frame_valid)
            {
                intrusion_operational = false;
                intrusion_init_error = intrusion_update_error;
            }
        }
        const bool intrusion_ready = intrusion_operational && tracker_operational;
        const char *intrusion_state = !is_intrusion_enabled(config) ? "disabled"
                                      : intrusion_ready               ? "running"
                                                                       : "unavailable";
        const bool intrusion_available = strcmp(intrusion_state, "running") == 0;
        const std::string intrusion_json = make_intrusion_json(
            config, intrusion_state, intrusion_frame_valid ? &intrusion_frame : NULL, update_ms,
            intrusion_available
                ? "intrusion evaluator running"
                : (intrusion_init_error.empty() ? "intrusion evaluator unavailable" : intrusion_init_error));
        std::string detection_json = make_detection_json(config, "running", frame_id,
                                                         frame_time_ms, update_ms, frame.cols, frame.rows,
                                                         &preprocess_transform,
                                                         &detection_views.display_detections,
                                                         detection_views,
                                                         &perf_stats);
        if (recorder != NULL)
        {
            AnalysisRecord record;
            record.recording_session_id = config.recording_config.recording_session_id;
            record.runtime_session_id = config.recording_config.runtime_session_id;
            record.capture_frame_id = frame_id;
            record.capture_timestamp_ms = frame_time_ms;
            record.record_timestamp_ms = update_ms;
            record.source_width = frame.cols;
            record.source_height = frame.rows;
            record.observations = policy_result.observations;
            record.tracker_input_observation_ids = policy_result.admitted_observation_ids;
            record.recorded_tracks = track_frame;
            record.effective_profile_id = config.profile_id;
            record.effective_profile_hash = config.profile_hash;
            record.timings.preprocess_ms = perf_stats.preprocess_ms;
            record.timings.inference_ms = perf_stats.inference_ms;
            record.timings.postprocess_ms = perf_stats.postprocess_ms;
            record.timings.policy_ms = policy_ms;
            record.timings.tracker_ms = tracker_diagnostics.last_update_ms;
            if (config.recording_config.frame_mode == "all" ||
                (config.recording_config.frame_mode == "sampled" && config.recording_config.jpeg_every_n > 0 &&
                 frame_id % config.recording_config.jpeg_every_n == 0))
                record.image_payload = make_recorded_frame_payload(frame);
            recorder->try_record(std::move(record));
        }
        RuntimeEvidenceAnalysisPublication publication;
        publication.frame_id = frame_id;
        publication.frame_time_ms = frame_time_ms;
        publication.update_ms = update_ms;
        publication.detection_count = detection_views.display_detections.count;
        publication.tracker_candidate_count = detection_views.tracker_candidates.count;
        publication.display_detection_count = detection_views.display_detections.count;
        publication.high_pass_detection_count = detection_views.high_pass_count;
        publication.low_pass_detection_count = detection_views.low_pass_count;
        publication.low_only_candidate_count = detection_views.low_only_count;
        publication.low_pass_high_duplicate_count = detection_views.low_pass_high_duplicate_count;
        publication.rejected_low_pass_high_score_count = detection_views.rejected_low_pass_high_score_count;
        publication.suppressed_low_only_overlap_count = detection_views.suppressed_low_only_overlap_count;
        publication.tracker_candidate_capacity_dropped_count = detection_views.capacity_dropped_count;
        publication.track_count = tracker_operational ? static_cast<int>(track_frame.objects.size()) : 0;
        publication.latest_track_frame_id = tracker_operational ? static_cast<int>(track_frame.capture_frame_id) : 0;
        publication.latest_track_capture_time_ms = tracker_operational ? track_frame.capture_timestamp_ms : 0;
        publication.tracker_diagnostics = tracker_diagnostics;
        publication.profile_admitted_last_frame = static_cast<int>(policy_result.admitted_observation_ids.size());
        publication.profile_rejected_last_frame = static_cast<int>(policy_result.observations.size() - policy_result.admitted_observation_ids.size());
        for (std::map<std::string, size_t>::const_iterator it = policy_result.rejected_by_reason.begin();
             it != policy_result.rejected_by_reason.end(); ++it)
        {
            publication.profile_rejected_by_reason[it->first] = static_cast<long long>(it->second);
        }
        publication.policy_ms = policy_ms;
        publication.tracker_state = tracker_state;
        publication.tracker_error = tracker_state == "error" ? tracker_message : "";
        publication.intrusion_state = intrusion_state;
        publication.intrusion_error = !intrusion_available ? intrusion_init_error : "";
        publication.detection_json = detection_json;
        publication.track_json = track_json;
        publication.intrusion_json = intrusion_json;
        publication.preprocess_ms = perf_stats.preprocess_ms;
        publication.inference_ms = perf_stats.inference_ms;
        publication.postprocess_ms = perf_stats.postprocess_ms;
        publication.preprocess_transform = preprocess_transform;
        publication.overlay_latency_ms = frame_time_ms > 0 ? update_ms - frame_time_ms : 0.0;
        publication.overlay_result.capture_frame_id = frame_id;
        publication.overlay_result.capture_time_ms = frame_time_ms;
        publication.overlay_result.update_time_ms = update_ms;
        for (int i = 0; i < detection_views.display_detections.count; ++i)
        {
            OverlayBox box;
            box.left = detection_views.display_detections.results[i].box.left;
            box.top = detection_views.display_detections.results[i].box.top;
            box.right = detection_views.display_detections.results[i].box.right;
            box.bottom = detection_views.display_detections.results[i].box.bottom;
            box.score = detection_views.display_detections.results[i].prop;
            box.name = detection_views.display_detections.results[i].name;
            publication.detection_boxes.push_back(box);
            publication.overlay_result.boxes.push_back(box);
        }
        if (recorder != NULL)
        {
            publication.has_recorder_diagnostics = true;
            publication.recorder_diagnostics = recorder->diagnostics();
        }
        stream_state->publish_analysis(publication);
    }

    release_runtime(ctx, model_data, NULL);
    stream_state->finish_inference();
}

struct RtspOutputContext
{
    const StreamServerConfig *config;
    RuntimeEvidence *stream_state;
    GMainLoop *loop;

    RtspOutputContext(const StreamServerConfig *cfg, RuntimeEvidence *state)
        : config(cfg),
          stream_state(state),
          loop(NULL)
    {
    }
};

static void on_rtsp_media_unprepared(GstRTSPMedia *media, gpointer user_data)
{
    (void)media;
    RtspOutputContext *ctx = (RtspOutputContext *)user_data;
    ctx->stream_state->mark_rtsp_media_unprepared();
}

static GstPadProbeReturn on_rtsp_frame_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
    {
        return GST_PAD_PROBE_OK;
    }

    RtspOutputContext *ctx = (RtspOutputContext *)user_data;
    long long now_ms = now_epoch_ms();
    ctx->stream_state->publish_clean_rtsp_frame(
        now_ms, std::string(stream_runtime_version()) + " GStreamer clean RTSP streaming");
    return GST_PAD_PROBE_OK;
}

static GstFlowReturn on_ai_appsink_new_sample(GstAppSink *appsink, gpointer user_data)
{
    RtspOutputContext *ctx = (RtspOutputContext *)user_data;
    GstSample *sample = gst_app_sink_pull_sample(appsink);
    if (!sample)
    {
        ctx->stream_state->record_ai_appsink_error("");
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    memset(&map, 0, sizeof(map));
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        gst_sample_unref(sample);
        ctx->stream_state->record_ai_appsink_error("");
        return GST_FLOW_OK;
    }

    const int width = ctx->config->width;
    const int height = ctx->config->height;
    const size_t expected_size = (size_t)width * (size_t)height * 3 / 2;
    cv::Mat ai_frame;
    bool copied = false;
    if (map.size >= expected_size)
    {
        cv::Mat nv12(height + height / 2, width, CV_8UC1, (void *)map.data);
        ai_frame = nv12.clone();
        copied = !ai_frame.empty();
    }
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    long long now_ms = now_epoch_ms();
    if (!copied)
    {
        ctx->stream_state->record_ai_appsink_error(
            std::string(stream_runtime_version()) + " AI appsink frame copy failed");
        return GST_FLOW_OK;
    }
    ctx->stream_state->publish_captured_frame(ai_frame, now_ms, width, height,
                                               static_cast<size_t>(ctx->config->frame_ring_size), true);
    return GST_FLOW_OK;
}

static bool configure_gstreamer_media(RtspOutputContext *ctx, GstElement *element)
{
    GstElement *probe_element = gst_bin_get_by_name_recurse_up(GST_BIN(element), "rtsp_h264parse");
    if (!probe_element)
    {
        probe_element = gst_bin_get_by_name_recurse_up(GST_BIN(element), "pay0");
    }
    if (!probe_element)
    {
        ctx->stream_state->record_rtsp_error(
            std::string(stream_runtime_version()) + " RTSP frame probe element not found");
        return false;
    }

    GstPad *src_pad = gst_element_get_static_pad(probe_element, "src");
    gst_object_unref(probe_element);
    if (!src_pad)
    {
        ctx->stream_state->record_rtsp_error(
            std::string(stream_runtime_version()) + " RTSP frame probe src pad not found");
        return false;
    }

    gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, on_rtsp_frame_buffer, ctx, NULL);
    gst_object_unref(src_pad);

    if (is_ai_branch_enabled(*ctx->config))
    {
        GstElement *appsink = gst_bin_get_by_name_recurse_up(GST_BIN(element), "ai_sink");
        if (!appsink)
        {
            ctx->stream_state->record_ai_appsink_error(
                std::string(stream_runtime_version()) + " AI appsink not found");
            return false;
        }
        g_object_set(G_OBJECT(appsink),
                     "emit-signals", TRUE,
                     "sync", FALSE,
                     "drop", TRUE,
                     "max-buffers", 1,
                     NULL);
        g_signal_connect(appsink, "new-sample", G_CALLBACK(on_ai_appsink_new_sample), ctx);
        gst_object_unref(appsink);
    }

    ctx->stream_state->mark_rtsp_ready(
        true, std::string(stream_runtime_version()) + " GStreamer clean RTSP media configured");
    return true;
}

static void on_rtsp_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
    (void)factory;
    RtspOutputContext *ctx = (RtspOutputContext *)user_data;
    GstElement *element = gst_rtsp_media_get_element(media);
    if (!element)
    {
        return;
    }

    configure_gstreamer_media(ctx, element);
    gst_object_unref(element);
    g_signal_connect(media, "unprepared", G_CALLBACK(on_rtsp_media_unprepared), ctx);
}

static void rtsp_server_loop(const StreamServerConfig config, RuntimeEvidence *stream_state)
{
    int argc = 0;
    char **argv = NULL;
    gst_init(&argc, &argv);

    RtspOutputContext ctx(&config, stream_state);
    ctx.loop = g_main_loop_new(NULL, FALSE);
    GstRTSPServer *server = gst_rtsp_server_new();
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
    std::string service = std::to_string(config.rtsp_port);
    std::string path = normalize_rtsp_path(config.rtsp_path);
    std::string launch = rtsp_launch_string(config);

    g_object_set(G_OBJECT(server), "service", service.c_str(), NULL);
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_rtsp_media_configure), &ctx);
    gst_rtsp_mount_points_add_factory(mounts, path.c_str(), factory);
    g_object_unref(mounts);

    guint attach_id = gst_rtsp_server_attach(server, NULL);
    stream_state->set_rtsp_server_state(attach_id != 0);
    if (attach_id == 0) stream_state->record_rtsp_error("");

    if (attach_id == 0)
    {
        printf("RTSP server attach failed: port=%d path=%s\n", config.rtsp_port, path.c_str());
        g_object_unref(server);
        g_main_loop_unref(ctx.loop);
        return;
    }

    printf("RTSP server started: url=%s launch=%s\n", make_rtsp_url(config).c_str(), launch.c_str());
    while (!g_stop_requested)
    {
        g_main_context_iteration(NULL, FALSE);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (attach_id != 0)
    {
        g_source_remove(attach_id);
    }
    stream_state->set_rtsp_server_state(false);
    g_object_unref(server);
    g_main_loop_unref(ctx.loop);
}

static void handle_client(int client_fd, const StreamServerConfig &config, RuntimeEvidence *stream_state)
{
    char buf[1024];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        close(client_fd);
        return;
    }
    buf[n] = '\0';
    std::string path = request_path(buf);
    if (path == "/status.json")
    {
        send_response(client_fd, 200, "OK", "application/json", make_status_json(config, stream_state));
    }
    else if (path == "/recording.json")
    {
        send_response(client_fd, 200, "OK", "application/json", make_recording_json(stream_state));
    }
    else if (path == "/healthz")
    {
        send_response(client_fd, 200, "OK", "text/plain", "ok\n");
    }
    else if (path == "/snapshot.jpg")
    {
        std::vector<unsigned char> jpeg;
        int frame_id = 0;
        if (make_snapshot_jpeg(stream_state, &jpeg, &frame_id))
        {
            (void)frame_id;
            send_binary_response(client_fd, "image/jpeg", jpeg);
        }
        else
        {
            send_response(client_fd, 503, "Service Unavailable", "text/plain", "no frame available\n");
        }
    }
    else if (path == "/detections.json")
    {
        send_response(client_fd, 200, "OK", "application/json", get_latest_detection_json(stream_state));
    }
    else if (path == "/tracks.json")
    {
        send_response(client_fd, 200, "OK", "application/json", get_latest_track_json(stream_state));
    }
    else if (path == "/events.json")
    {
        send_response(client_fd, 200, "OK", "application/json", get_latest_intrusion_json(stream_state));
    }
    else if (path == "/" || path == "/index.html")
    {
        send_response(client_fd, 200, "OK", "text/plain",
                      "RK3568 rknn_detect V4.0.0 stream server\n"
                      "GET /snapshot.jpg\n"
                      "GET /detections.json\n"
                      "GET /tracks.json\n"
                      "GET /events.json\n"
                      "GET /status.json\n"
                      "GET /recording.json\n"
                      "GET /healthz\n");
    }
    else
    {
        send_response(client_fd, 404, "Not Found", "text/plain", "not found\n");
    }
    close(client_fd);
}

static int create_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        printf("socket failed: %s\n", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("bind port %d failed: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0)
    {
        printf("listen failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    StreamServerConfig config;
    config.port = 8080;
    config.max_seconds = 0;
    config.video_node = "/dev/video0";
    config.width = 640;
    config.height = 480;
    config.fps = 30;
    config.inference_fps = 10;
    config.rtsp_port = 8554;
    config.rtsp_path = "/rknn_detect";
    config.rtsp_fps = 25;
    config.h264_bitrate = 2000000;
    config.display_mode = LOCAL_DISPLAY_DISABLED;
    config.output_delay_ms = 250;
    config.latest_hold_ms = 300;
    config.frame_ring_size = 60;
    config.stale_threshold_ms = 1000;
    config.box_conf_threshold = BOX_THRESH;
    config.nms_threshold = NMS_THRESH;
    config.tracker_type = STREAM_TRACKER_NONE;
    config.tracker_config = TrackerConfig();
    config.tracker_config.low_threshold = 0.35f;
    config.profile_id = "default-general";
    config.profile_hash = "";
    config.observation_policy_config = ObservationPolicyConfig();
    config.intrusion_schema_version = 1;
    config.intrusion_camera_id = "camera0";
    config.intrusion_config = IntrusionRuleConfig();
    config.intrusion_config_supplied = false;
    config.recording_enabled = false;
    config.recording_config = RecordingSessionConfig();
    config.preprocess_mode = PREPROCESS_MODE_LETTERBOX;
    config.pixel_format = "NV12";

    bool tracker_type_explicit = false;
    bool tracker_high_threshold_explicit = false;
    bool tracker_new_track_threshold_explicit = false;
    bool intrusion_enable_explicit = false;
    std::vector<char *> positional_args;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--port") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--port", argv[++i], 1, 65535, &config.port))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--max-seconds") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--max-seconds", argv[++i], 0, 86400, &config.max_seconds))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--video-node") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return -1;
            }
            config.video_node = argv[++i];
        }
        else if (strcmp(argv[i], "--width") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--width", argv[++i], 1, 10000, &config.width))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--height") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--height", argv[++i], 1, 10000, &config.height))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--fps") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--fps", argv[++i], 1, 240, &config.fps))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--inference-fps") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--inference-fps", argv[++i], 1, 60, &config.inference_fps))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--rtsp-port") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--rtsp-port", argv[++i], 1, 65535, &config.rtsp_port))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--rtsp-path") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return -1;
            }
            config.rtsp_path = normalize_rtsp_path(argv[++i]);
        }
        else if (strcmp(argv[i], "--rtsp-fps") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--rtsp-fps", argv[++i], 1, 60, &config.rtsp_fps))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--h264-bitrate") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--h264-bitrate", argv[++i], 10000, 100000000, &config.h264_bitrate))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--display-mode") == 0)
        {
            if (i + 1 >= argc || !parse_local_display_mode(argv[++i], &config.display_mode))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--output-delay-ms") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--output-delay-ms", argv[++i], 0, 60000, &config.output_delay_ms))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--latest-hold-ms") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--latest-hold-ms", argv[++i], 0, 60000, &config.latest_hold_ms))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--frame-ring-size") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--frame-ring-size", argv[++i], 1, 1000, &config.frame_ring_size))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--stale-threshold-ms") == 0)
        {
            if (i + 1 >= argc ||
                !parse_int_arg("--stale-threshold-ms", argv[++i], 0, 60000, &config.stale_threshold_ms))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--preprocess-mode") == 0)
        {
            if (i + 1 >= argc || !parse_preprocess_mode(argv[++i], &config.preprocess_mode))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--box-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--box-thresh", argv[++i], 0.001f, 0.999f, &config.box_conf_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--nms-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--nms-thresh", argv[++i], 0.001f, 0.999f, &config.nms_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-type") == 0)
        {
            if (i + 1 >= argc || !parse_stream_tracker_type(argv[++i], &config.tracker_type))
            {
                print_usage(argv[0]);
                return -1;
            }
            tracker_type_explicit = true;
        }
        else if (strcmp(argv[i], "--tracker-low-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--tracker-low-thresh", argv[++i], 0.0f, 0.999f,
                                 &config.tracker_config.low_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-high-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--tracker-high-thresh", argv[++i], 0.001f, 1.0f,
                                 &config.tracker_config.high_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
            tracker_high_threshold_explicit = true;
        }
        else if (strcmp(argv[i], "--tracker-new-track-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--tracker-new-track-thresh", argv[++i], 0.001f, 1.0f,
                                 &config.tracker_config.new_track_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
            tracker_new_track_threshold_explicit = true;
        }
        else if (strcmp(argv[i], "--tracker-match-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--tracker-match-thresh", argv[++i], 0.001f, 1.0f,
                                 &config.tracker_config.match_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-second-match-thresh") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--tracker-second-match-thresh", argv[++i], 0.001f, 1.0f,
                                 &config.tracker_config.second_match_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-confirm-hits") == 0)
        {
            if (i + 1 >= argc ||
                !parse_int_arg("--tracker-confirm-hits", argv[++i], 1, 100,
                               &config.tracker_config.confirm_hits))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-lost-timeout-ms") == 0)
        {
            if (i + 1 >= argc ||
                !parse_int_arg("--tracker-lost-timeout-ms", argv[++i], 1, 60000,
                               &config.tracker_config.lost_timeout_ms))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--tracker-max-tracks") == 0)
        {
            int max_tracks = 0;
            if (i + 1 >= argc || !parse_int_arg("--tracker-max-tracks", argv[++i], 1,
                                                OBJ_NUMB_MAX_SIZE, &max_tracks))
            {
                print_usage(argv[0]);
                return -1;
            }
            config.tracker_config.max_tracks = static_cast<size_t>(max_tracks);
        }
        else if (strcmp(argv[i], "--profile-id") == 0)
        {
            if (i + 1 >= argc || strlen(argv[i + 1]) == 0) { print_usage(argv[0]); return -1; }
            config.profile_id = argv[++i];
        }
        else if (strcmp(argv[i], "--profile-hash") == 0)
        {
            if (i + 1 >= argc || strncmp(argv[i + 1], "sha256:", 7) != 0 || strlen(argv[i + 1]) != 71)
            {
                printf("--profile-hash must be sha256 followed by 64 lowercase hex characters\n");
                return -1;
            }
            config.profile_hash = argv[++i];
            for (size_t j = 7; j < config.profile_hash.size(); ++j)
            {
                if (!((config.profile_hash[j] >= '0' && config.profile_hash[j] <= '9') ||
                      (config.profile_hash[j] >= 'a' && config.profile_hash[j] <= 'f')))
                { printf("--profile-hash must use lowercase hexadecimal\n"); return -1; }
            }
        }
        else if (strcmp(argv[i], "--tracker-class-ids") == 0)
        {
            if (i + 1 >= argc ||
                !parse_class_ids("--tracker-class-ids", argv[++i], &config.observation_policy_config.allowed_class_ids))
                return -1;
        }
        else if (strcmp(argv[i], "--tracker-min-width") == 0)
        {
            if (i + 1 >= argc || !parse_float_arg("--tracker-min-width", argv[++i], 0.0f, 10000.0f, &config.observation_policy_config.min_width)) return -1;
        }
        else if (strcmp(argv[i], "--tracker-min-height") == 0)
        {
            if (i + 1 >= argc || !parse_float_arg("--tracker-min-height", argv[++i], 0.0f, 10000.0f, &config.observation_policy_config.min_height)) return -1;
        }
        else if (strcmp(argv[i], "--tracker-min-area") == 0)
        {
            if (i + 1 >= argc || !parse_float_arg("--tracker-min-area", argv[++i], 0.0f, 100000000.0f, &config.observation_policy_config.min_area)) return -1;
        }
        else if (strcmp(argv[i], "--tracker-edge-margin") == 0)
        {
            if (i + 1 >= argc || !parse_float_arg("--tracker-edge-margin", argv[++i], 0.0f, 10000.0f, &config.observation_policy_config.edge_margin)) return -1;
        }
        else if (strcmp(argv[i], "--tracker-roi") == 0)
        {
            if (i + 1 >= argc || !parse_tracker_roi(argv[++i], &config.observation_policy_config.roi)) return -1;
        }
        else if (strcmp(argv[i], "--intrusion-enabled") == 0)
        {
            config.intrusion_config.enabled = true;
            config.intrusion_config_supplied = true;
            intrusion_enable_explicit = true;
        }
        else if (strcmp(argv[i], "--intrusion-disabled") == 0)
        {
            config.intrusion_config.enabled = false;
            config.intrusion_config_supplied = true;
            intrusion_enable_explicit = true;
        }
        else if (strcmp(argv[i], "--intrusion-schema-version") == 0)
        {
            if (i + 1 >= argc ||
                !parse_int_arg("--intrusion-schema-version", argv[++i], 1, 1,
                               &config.intrusion_schema_version))
                return -1;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-camera-id") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1;
            config.intrusion_camera_id = argv[++i];
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-rule-id") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1;
            config.intrusion_config.rule_id = argv[++i];
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-class-ids") == 0)
        {
            if (i + 1 >= argc ||
                !parse_class_ids("--intrusion-class-ids", argv[++i], &config.intrusion_config.class_ids))
                return -1;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-region") == 0)
        {
            if (i + 1 >= argc || !parse_intrusion_region(argv[++i], &config.intrusion_config.region))
                return -1;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-dwell-ms") == 0)
        {
            int value = 0;
            if (i + 1 >= argc ||
                !parse_int_arg("--intrusion-dwell-ms", argv[++i], 1, 86400000, &value))
                return -1;
            config.intrusion_config.dwell_ms = value;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-boundary-hysteresis-px") == 0)
        {
            if (i + 1 >= argc ||
                !parse_float_arg("--intrusion-boundary-hysteresis-px", argv[++i], 0.0f, 10000.0f,
                                 &config.intrusion_config.boundary_hysteresis_px))
                return -1;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--intrusion-prediction-grace-ms") == 0)
        {
            int value = 0;
            if (i + 1 >= argc ||
                !parse_int_arg("--intrusion-prediction-grace-ms", argv[++i], 0, 60000, &value))
                return -1;
            config.intrusion_config.prediction_grace_ms = value;
            config.intrusion_config_supplied = true;
        }
        else if (strcmp(argv[i], "--record-analysis") == 0)
        {
            config.recording_enabled = true;
        }
        else if (strcmp(argv[i], "--recording-root") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1; config.recording_config.recording_root = argv[++i];
        }
        else if (strcmp(argv[i], "--recording-session-id") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1; config.recording_config.recording_session_id = argv[++i];
        }
        else if (strcmp(argv[i], "--runtime-session-id") == 0)
        {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') return -1; config.recording_config.runtime_session_id = argv[++i];
        }
        else if (strcmp(argv[i], "--recording-queue-capacity") == 0)
        {
            int value = 0; if (i + 1 >= argc || !parse_int_arg("--recording-queue-capacity", argv[++i], 1, 4096, &value)) return -1; config.recording_config.queue_capacity_records = value;
        }
        else if (strcmp(argv[i], "--recording-segment-mb") == 0)
        {
            int value = 0; if (i + 1 >= argc || !parse_int_arg("--recording-segment-mb", argv[++i], 1, 4096, &value)) return -1; config.recording_config.segment_max_bytes = static_cast<size_t>(value) * 1024U * 1024U;
        }
        else if (strcmp(argv[i], "--recording-session-mb") == 0)
        {
            int value = 0; if (i + 1 >= argc || !parse_int_arg("--recording-session-mb", argv[++i], 1, 65536, &value)) return -1; config.recording_config.session_max_bytes = static_cast<size_t>(value) * 1024U * 1024U;
        }
        else if (strcmp(argv[i], "--recording-max-seconds") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--recording-max-seconds", argv[++i], 1, 86400, &config.recording_config.session_max_duration_s)) return -1;
        }
        else if (strcmp(argv[i], "--recording-min-free-mb") == 0)
        {
            int value = 0; if (i + 1 >= argc || !parse_int_arg("--recording-min-free-mb", argv[++i], 0, 65536, &value)) return -1; config.recording_config.min_free_bytes = static_cast<size_t>(value) * 1024U * 1024U;
        }
        else if (strcmp(argv[i], "--record-frame-mode") == 0)
        {
            if (i + 1 >= argc || (strcmp(argv[i + 1], "none") != 0 && strcmp(argv[i + 1], "sampled") != 0 && strcmp(argv[i + 1], "all") != 0)) return -1;
            config.recording_config.frame_mode = argv[++i];
        }
        else if (strcmp(argv[i], "--record-jpeg-every") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--record-jpeg-every", argv[++i], 1, 100000, &config.recording_config.jpeg_every_n)) return -1;
        }
        else if (strcmp(argv[i], "--record-jpeg-quality") == 0)
        {
            if (i + 1 >= argc || !parse_int_arg("--record-jpeg-quality", argv[++i], 1, 100, &config.recording_config.jpeg_quality)) return -1;
        }
        else if (strcmp(argv[i], "--record-image-pool-capacity") == 0)
        {
            int value = 0; if (i + 1 >= argc || !parse_int_arg("--record-image-pool-capacity", argv[++i], 1, 256, &value)) return -1;
            config.recording_config.image_pool_capacity = static_cast<size_t>(value);
        }
        else if (strcmp(argv[i], "--pixel-format") == 0)
        {
            if (i + 1 >= argc)
            {
                print_usage(argv[0]);
                return -1;
            }
            config.pixel_format = argv[++i];
        }
        else
        {
            if (argv[i][0] == '-')
            {
                printf("unknown option '%s'\n", argv[i]);
                print_usage(argv[0]);
                return -1;
            }
            positional_args.push_back(argv[i]);
        }
    }

    if (positional_args.size() > 1)
    {
        print_usage(argv[0]);
        return -1;
    }
    if (positional_args.size() == 1)
    {
        config.model_path = positional_args[0];
    }
    if (!tracker_type_explicit)
    {
        config.tracker_type = STREAM_TRACKER_BYTETRACK;
    }
    if (!tracker_high_threshold_explicit)
    {
        config.tracker_config.high_threshold = config.box_conf_threshold;
    }
    if (!tracker_new_track_threshold_explicit)
    {
        config.tracker_config.new_track_threshold = config.tracker_config.high_threshold;
    }
    ResolvedTrackingProfile effective;
    effective.profile_id = config.profile_id;
    effective.detector_high_threshold = config.box_conf_threshold;
    effective.detector_nms_threshold = config.nms_threshold;
    effective.observation = config.observation_policy_config;
    effective.tracker_type = stream_tracker_type_to_string(config.tracker_type);
    effective.tracker = config.tracker_config;
    if (effective.tracker_type == "none") effective.tracker_type = "bytetrack";
    const std::string calculated_profile_hash = hash_canonical_tracking_profile(effective);
    if (!config.profile_hash.empty() && config.profile_hash != calculated_profile_hash)
    {
        printf("--profile-hash does not match effective CLI configuration: calculated %s\n",
               calculated_profile_hash.c_str());
        return -1;
    }
    config.profile_hash = calculated_profile_hash;
    config.tracker_config.nominal_interval_ms = std::max(1, 1000 / config.inference_fps);
    config.tracker_config.max_timestamp_gap_ms = std::max(2000, config.tracker_config.lost_timeout_ms * 2);
    if (config.observation_policy_config.edge_margin * 2.0f >= config.width ||
        config.observation_policy_config.edge_margin * 2.0f >= config.height)
    {
        printf("--tracker-edge-margin must leave a non-empty source region\n");
        return -1;
    }
    std::string tracker_config_error;
    if (config.tracker_type == STREAM_TRACKER_BYTETRACK &&
        !validate_tracker_config(config.tracker_config, &tracker_config_error))
    {
        printf("invalid tracker configuration: %s\n", tracker_config_error.c_str());
        return -1;
    }
    if (config.intrusion_config_supplied && !intrusion_enable_explicit)
    {
        printf("intrusion configuration must explicitly select --intrusion-enabled or --intrusion-disabled\n");
        return -1;
    }
    if (config.intrusion_config_supplied && config.intrusion_camera_id != "camera0")
    {
        printf("--intrusion-camera-id must be camera0 for the single-camera demo\n");
        return -1;
    }
    if (is_intrusion_enabled(config))
    {
        std::string intrusion_config_error;
        if (!validate_intrusion_rule_config(config.intrusion_config, &intrusion_config_error))
        {
            printf("invalid intrusion configuration: %s\n", intrusion_config_error.c_str());
            return -1;
        }
        if (!is_tracking_enabled(config))
        {
            printf("--intrusion-enabled requires ByteTrack and a model\n");
            return -1;
        }
    }
    if (config.recording_config.runtime_session_id.empty())
        config.recording_config.runtime_session_id = std::to_string(now_epoch_ms()) + "-" + std::to_string(getpid());
    if (!safe_session_id(config.recording_config.runtime_session_id))
    {
        printf("--runtime-session-id contains unsafe path characters\n");
        return -1;
    }
    if (config.recording_config.recording_session_id.empty())
        config.recording_config.recording_session_id = config.recording_config.runtime_session_id + "-camera0";
    if (!safe_session_id(config.recording_config.recording_session_id))
    {
        printf("--recording-session-id contains unsafe path characters\n");
        return -1;
    }
    if (config.recording_config.segment_max_bytes > config.recording_config.session_max_bytes)
    {
        printf("recording segment limit must not exceed session limit\n");
        return -1;
    }
    if (config.recording_enabled && (!is_tracking_enabled(config) || config.model_path.empty()))
    {
        printf("--record-analysis requires an enabled tracker and model\n");
        return -1;
    }
    config.recording_config.git_commit = RKNN_DETECT_GIT_COMMIT;
    config.recording_config.model_path = config.model_path;
    config.recording_config.model_hash = config.model_path.empty() ? "unavailable" : sha256_file(config.model_path);
    config.recording_config.labels_hash = sha256_file("model/coco_80_labels_list.txt");
    config.recording_config.effective_profile_id = config.profile_id;
    config.recording_config.effective_profile_hash = config.profile_hash;
    config.recording_config.resolved_profile_json = canonical_tracking_profile_json(effective);
    config.recording_config.source_width = config.width;
    config.recording_config.source_height = config.height;
    config.recording_config.preprocess_mode = preprocess_mode_to_string(config.preprocess_mode);
    const int record_jpeg_quality = config.recording_config.jpeg_quality;
    config.recording_config.image_encoder = [record_jpeg_quality](const RecordedFramePayload &payload,
                                                                                              std::vector<unsigned char> *jpeg,
                                                                                              std::string *error) -> bool {
        if (payload.width <= 0 || payload.height <= 0 || payload.bgr.empty()) { if (error) *error = "empty image payload"; return false; }
        cv::Mat image(payload.height, payload.width, CV_8UC3, const_cast<unsigned char *>(payload.bgr.data()));
        std::vector<int> params; params.push_back(cv::IMWRITE_JPEG_QUALITY); params.push_back(record_jpeg_quality);
        if (!cv::imencode(".jpg", image, *jpeg, params)) { if (error) *error = "OpenCV JPEG encode failed"; return false; }
        if (error) error->clear(); return true;
    };
    configure_mpp_rga_policy();
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    RuntimeEvidence stream_state;
    initialize_intrusion_state(config, &stream_state);
    std::unique_ptr<IAnalysisRecorder> analysis_recorder;
    if (config.recording_enabled)
    {
        analysis_recorder = create_analysis_recorder();
        std::string recording_error;
        if (!analysis_recorder->start(config.recording_config, &recording_error))
        {
            printf("analysis recording disabled after start failure: %s\n", recording_error.c_str());
            stream_state.set_recorder_diagnostics(analysis_recorder->diagnostics());
            config.recording_enabled = false;
            analysis_recorder.reset();
        }
        else
        {
            stream_state.set_recorder_diagnostics(analysis_recorder->diagnostics());
        }
    }
    int server_fd = create_server_socket(config.port);
    if (server_fd < 0)
    {
        return -1;
    }

    printf("%s stream server started: http_port=%d rtsp_url=%s display_mode=%s tracker_type=%s tracker_low=%.3f tracker_high=%.3f video_node=%s size=%dx%d fps=%d rtsp_fps=%d inference_fps=%d output_delay_ms=%d latest_hold_ms=%d frame_ring_size=%d stale_threshold_ms=%d format=%s model_type=yolov6 preprocess_mode=%s model=%s max_seconds=%d\n",
           stream_runtime_version(),
           config.port, make_rtsp_url(config).c_str(),
           local_display_mode_to_string(config.display_mode),
           stream_tracker_type_to_string(config.tracker_type),
           config.tracker_config.low_threshold,
           config.tracker_config.high_threshold,
           config.video_node.c_str(),
           config.width, config.height, config.fps, config.rtsp_fps,
           config.inference_fps, config.output_delay_ms, config.latest_hold_ms, config.frame_ring_size,
           config.stale_threshold_ms, config.pixel_format.c_str(),
           preprocess_mode_to_string(config.preprocess_mode),
           config.model_path.empty() ? "none" : config.model_path.c_str(), config.max_seconds);

    IAnalysisRecorder *recorder = analysis_recorder.get();
    StreamPipelineHooks pipeline_hooks;
    pipeline_hooks.has_model = !config.model_path.empty();
    pipeline_hooks.initialize_gstreamer = [config, &stream_state]() {
        initialize_gstreamer_state(config, &stream_state);
    };
    pipeline_hooks.inference_loop = [config, &stream_state, recorder]() {
        inference_loop(config, &stream_state, recorder);
    };
    pipeline_hooks.rtsp_loop = [config, &stream_state]() {
        rtsp_server_loop(config, &stream_state);
    };
    pipeline_hooks.notify_waiters = [&stream_state]() {
        stream_state.notify_waiters();
    };
    pipeline_hooks.stop_pipeline = []() {
        g_stop_requested = 1;
    };
    StreamPipeline pipeline(pipeline_hooks);
    pipeline.start();
    time_t start_time = time(NULL);
    while (!g_stop_requested)
    {
        if (config.max_seconds > 0 && (time(NULL) - start_time) >= config.max_seconds)
        {
            g_stop_requested = 1;
            pipeline.request_stop();
            break;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int ret = select(server_fd + 1, &fds, NULL, NULL, &timeout);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            printf("select failed: %s\n", strerror(errno));
            g_stop_requested = 1;
            pipeline.request_stop();
            break;
        }
        if (ret == 0)
        {
            continue;
        }

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            printf("accept failed: %s\n", strerror(errno));
            continue;
        }
        std::thread(handle_client, client_fd, config, &stream_state).detach();
    }

    close(server_fd);
    pipeline.request_stop();
    pipeline.join();
    if (analysis_recorder.get() != NULL)
    {
        std::string recording_error;
        analysis_recorder->stop(RecorderStopMode::Drain, &recording_error);
        stream_state.set_recorder_diagnostics(analysis_recorder->diagnostics());
    }
    stream_state.mark_pipeline_stopped(g_stop_requested != 0);
    printf("%s stream server stopped: stop_requested=%s\n",
           stream_runtime_version(),
           g_stop_requested ? "yes" : "no");
    return 0;
}
