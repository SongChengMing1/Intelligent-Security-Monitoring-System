#ifndef RKNN_DETECT_ANALYSIS_RECORDER_H_
#define RKNN_DETECT_ANALYSIS_RECORDER_H_

#include <stddef.h>

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "analysis_record.h"

enum class RecorderState
{
    Disabled,
    Starting,
    Recording,
    Finalizing,
    Finalized,
    LimitReached,
    Failed,
    Interrupted,
};

enum class RecordEnqueueResult
{
    Accepted,
    Disabled,
    QueueFull,
    Stopped,
};

enum class RecorderStopMode
{
    Drain,
    Immediate,
};

struct RecordingSessionConfig
{
    std::string recording_root;
    std::string recording_session_id;
    std::string runtime_session_id;
    std::string git_commit;
    std::string model_path;
    std::string model_hash;
    std::string labels_hash;
    std::string effective_profile_id;
    std::string effective_profile_hash;
    std::string resolved_profile_json;
    int source_width;
    int source_height;
    std::string preprocess_mode;
    size_t queue_capacity_records;
    size_t segment_max_bytes;
    size_t session_max_bytes;
    int session_max_duration_s;
    size_t min_free_bytes;
    int drain_timeout_ms;
    std::string frame_mode;
    int jpeg_every_n;
    int jpeg_quality;
    size_t image_pool_capacity;
    std::function<bool(const RecordedFramePayload &, std::vector<unsigned char> *, std::string *)> image_encoder;

    RecordingSessionConfig();
};

struct RecorderDiagnostics
{
    RecorderState state;
    std::string recording_session_id;
    std::string session_path;
    size_t queue_size;
    size_t queue_capacity;
    long long written_records;
    long long dropped_records;
    long long dropped_images;
    long long shutdown_dropped_records;
    size_t bytes_written;
    int current_segment;
    double last_enqueue_ms;
    double max_enqueue_ms;
    std::string error;

    RecorderDiagnostics();
};

const char *recorder_state_to_string(RecorderState state);

class IAnalysisRecorder
{
public:
    virtual ~IAnalysisRecorder() {}
    virtual bool start(const RecordingSessionConfig &config, std::string *error_message) = 0;
    virtual RecordEnqueueResult try_record(AnalysisRecord &&record) = 0;
    virtual bool stop(RecorderStopMode mode, std::string *error_message) = 0;
    virtual RecorderDiagnostics diagnostics() const = 0;
};

std::unique_ptr<IAnalysisRecorder> create_analysis_recorder();

struct SessionReadResult
{
    std::vector<AnalysisRecord> records;
    bool ignored_truncated_final_line;
    std::string error;

    SessionReadResult() : records(), ignored_truncated_final_line(false), error() {}
};

bool read_analysis_session(const std::string &session_path, SessionReadResult *result);
bool mark_incomplete_session_interrupted(const std::string &session_path, std::string *error_message);

#endif
