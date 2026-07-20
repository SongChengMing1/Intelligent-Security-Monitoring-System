#include <unistd.h>
#include <sys/stat.h>

#include <fstream>
#include <chrono>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#include "analysis_recorder.h"
#include <nlohmann/json.hpp>

namespace
{
bool expect(bool condition, const std::string &message) { if (!condition) std::cerr << "FAIL: " << message << std::endl; return condition; }

AnalysisRecord make_record(long long frame_id)
{
    AnalysisRecord record; record.recording_session_id = "fixture"; record.runtime_session_id = "runtime";
    record.capture_frame_id = frame_id; record.capture_timestamp_ms = 1000 + frame_id; record.record_timestamp_ms = 2000 + frame_id;
    record.source_width = 640; record.source_height = 480; record.effective_profile_id = "default-general";
    record.effective_profile_hash = "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    record.recorded_tracks.capture_frame_id = frame_id; record.recorded_tracks.capture_timestamp_ms = 1000 + frame_id;
    AnalysisObservation observation; observation.observation_id = 1; observation.class_id = 0; observation.class_name = "person";
    observation.score = 0.8f; observation.bbox.x1 = 1; observation.bbox.y1 = 2; observation.bbox.x2 = 20; observation.bbox.y2 = 40; observation.admitted = true;
    record.observations.push_back(observation); record.tracker_input_observation_ids.push_back(1); return record;
}

RecordingSessionConfig make_config(const std::string &root, const std::string &session)
{
    RecordingSessionConfig config; config.recording_root = root; config.recording_session_id = session; config.runtime_session_id = "runtime";
    config.effective_profile_id = "default-general"; config.effective_profile_hash = "sha256:fixture"; config.resolved_profile_json = "{}";
    config.source_width = 640; config.source_height = 480; config.preprocess_mode = "letterbox"; config.min_free_bytes = 0;
    config.segment_max_bytes = 1200; config.session_max_bytes = 100000; return config;
}
} // namespace

int main()
{
    bool ok = true; std::ostringstream root; root << "/tmp/rknn_detect_v351_recorder_" << getpid() << "_"
        << std::chrono::steady_clock::now().time_since_epoch().count();
    RecordingSessionConfig config = make_config(root.str(), "fixture"); std::unique_ptr<IAnalysisRecorder> recorder = create_analysis_recorder(); std::string error;
    ok &= expect(recorder->start(config, &error), "recorder starts: " + error);
    for (int i = 1; i <= 8; ++i) recorder->try_record(make_record(i));
    ok &= expect(recorder->stop(RecorderStopMode::Drain, &error), "drain stop succeeds: " + error);
    RecorderDiagnostics diagnostics = recorder->diagnostics();
    ok &= expect(diagnostics.state == RecorderState::Finalized, "final state");
    ok &= expect(diagnostics.written_records + diagnostics.dropped_records == 8, "all submissions accounted");
    ok &= expect(diagnostics.current_segment >= 2, "segments rotate");
    ok &= expect(diagnostics.max_enqueue_ms < 10.0, "enqueue remains bounded in Host test");
    std::ifstream status_input((diagnostics.session_path + "/session-status.json").c_str()); nlohmann::json final_status; status_input >> final_status;
    ok &= expect(final_status.at("state") == "finalized", "atomic final status is readable");
    ok &= expect(final_status.at("segments").size() == static_cast<size_t>(diagnostics.current_segment), "per-segment accounting is complete");
    std::unique_ptr<IAnalysisRecorder> duplicate_writer = create_analysis_recorder();
    ok &= expect(!duplicate_writer->start(config, &error), "existing session cannot be appended or locked twice");
    SessionReadResult read; ok &= expect(read_analysis_session(diagnostics.session_path, &read), "session reads");
    ok &= expect(static_cast<long long>(read.records.size()) == diagnostics.written_records, "read count matches");

    std::ofstream truncated((diagnostics.session_path + "/records-" + std::string("000") +
                             (diagnostics.current_segment < 10 ? "00" : "0") + std::to_string(diagnostics.current_segment) + ".jsonl").c_str(), std::ios::app);
    truncated << "{truncated"; truncated.close();
    SessionReadResult truncated_read; ok &= expect(read_analysis_session(diagnostics.session_path, &truncated_read), "final truncated line tolerated");
    ok &= expect(truncated_read.ignored_truncated_final_line, "truncation reported");
    std::ofstream corrupt((diagnostics.session_path + "/records-" + std::string("000") +
                           (diagnostics.current_segment < 10 ? "00" : "0") + std::to_string(diagnostics.current_segment) + ".jsonl").c_str(), std::ios::app);
    corrupt << "\n"; corrupt.close();
    SessionReadResult corrupt_read; ok &= expect(!read_analysis_session(diagnostics.session_path, &corrupt_read), "newline-complete corruption is rejected");

    std::unique_ptr<IAnalysisRecorder> low_space = create_analysis_recorder(); RecordingSessionConfig low = make_config(root.str(), "low-space");
    low.min_free_bytes = std::numeric_limits<size_t>::max(); ok &= expect(low_space->start(low, &error), "low-space recorder starts");
    low_space->try_record(make_record(1)); low_space->stop(RecorderStopMode::Drain, &error);
    ok &= expect(low_space->diagnostics().state == RecorderState::LimitReached, "minimum free limit stops session");

    std::unique_ptr<IAnalysisRecorder> byte_limit = create_analysis_recorder(); RecordingSessionConfig tiny = make_config(root.str(), "byte-limit");
    tiny.segment_max_bytes = 1; tiny.session_max_bytes = 1; ok &= expect(byte_limit->start(tiny, &error), "byte-limit recorder starts");
    byte_limit->try_record(make_record(1)); byte_limit->stop(RecorderStopMode::Drain, &error);
    ok &= expect(byte_limit->diagnostics().state == RecorderState::LimitReached, "session byte limit stops session");

    std::unique_ptr<IAnalysisRecorder> invalid = create_analysis_recorder(); RecordingSessionConfig bad = make_config("/proc/rknn-detect", "bad");
    ok &= expect(!invalid->start(bad, &error), "unwritable root fails start");
    std::unique_ptr<IAnalysisRecorder> invalid_json = create_analysis_recorder(); RecordingSessionConfig invalid_json_config = make_config(root.str(), "invalid-json");
    invalid_json_config.resolved_profile_json = "{"; ok &= expect(!invalid_json->start(invalid_json_config, &error), "serialization setup failure is isolated");

    std::unique_ptr<IAnalysisRecorder> bounded = create_analysis_recorder(); RecordingSessionConfig bounded_config = make_config(root.str(), "bounded");
    bounded_config.queue_capacity_records = 1; ok &= expect(bounded->start(bounded_config, &error), "bounded recorder starts");
    int queue_full = 0; for (int i = 0; i < 1000; ++i) if (bounded->try_record(make_record(i + 1)) == RecordEnqueueResult::QueueFull) ++queue_full;
    bounded->stop(RecorderStopMode::Immediate, &error); ok &= expect(queue_full > 0, "queue-full drop is observable");

    std::unique_ptr<IAnalysisRecorder> timeout = create_analysis_recorder(); RecordingSessionConfig timeout_config = make_config(root.str(), "drain-timeout");
    timeout_config.queue_capacity_records = 4096; timeout_config.drain_timeout_ms = 0; ok &= expect(timeout->start(timeout_config, &error), "timeout recorder starts");
    for (int i = 0; i < 1000; ++i) timeout->try_record(make_record(i + 1)); timeout->stop(RecorderStopMode::Drain, &error);
    ok &= expect(timeout->diagnostics().shutdown_dropped_records > 0, "bounded drain accounts timeout drops");

    std::unique_ptr<IAnalysisRecorder> images = create_analysis_recorder(); RecordingSessionConfig image_config = make_config(root.str(), "images");
    image_config.frame_mode = "sampled"; image_config.jpeg_every_n = 2; image_config.image_pool_capacity = 1;
    image_config.image_encoder = [](const RecordedFramePayload &payload, std::vector<unsigned char> *jpeg, std::string *image_error) {
        if (payload.width != 2 || payload.height != 2) { if (image_error) *image_error = "unexpected fixture dimensions"; return false; }
        jpeg->assign({0xff, 0xd8, 0xff, 0xd9}); if (image_error) image_error->clear(); return true;
    };
    ok &= expect(images->start(image_config, &error), "image recorder starts");
    AnalysisRecord image_record = make_record(2); image_record.image_payload.reset(new RecordedFramePayload()); image_record.image_payload->width = 2; image_record.image_payload->height = 2; image_record.image_payload->bgr.resize(12);
    images->try_record(std::move(image_record)); images->stop(RecorderStopMode::Drain, &error);
    SessionReadResult image_read; ok &= expect(read_analysis_session(images->diagnostics().session_path, &image_read), "image session reads");
    ok &= expect(!image_read.records.empty() && image_read.records[0].image.state == RecordedImageState::Exact, "sampled image is exact");

    const std::string interrupted_path = root.str() + "/interrupted"; mkdir(interrupted_path.c_str(), 0755);
    std::ofstream status((interrupted_path + "/session-status.json").c_str());
    status << "{\"status_schema_version\":1,\"state\":\"recording\",\"recording_session_id\":\"interrupted\",\"written_records\":0,\"dropped_records\":0,\"bytes_written\":0,\"segments\":[]}"; status.close();
    ok &= expect(mark_incomplete_session_interrupted(interrupted_path, &error), "incomplete session is marked interrupted");

    const int benchmark_records = 2000;
    std::unique_ptr<IAnalysisRecorder> disabled = create_analysis_recorder();
    std::chrono::steady_clock::time_point disabled_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < benchmark_records; ++i) disabled->try_record(make_record(i + 1));
    const double disabled_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - disabled_begin).count();
    std::unique_ptr<IAnalysisRecorder> benchmark = create_analysis_recorder(); RecordingSessionConfig benchmark_config = make_config(root.str(), "benchmark");
    benchmark_config.queue_capacity_records = 4096; benchmark_config.segment_max_bytes = 1024 * 1024; benchmark_config.session_max_bytes = 16 * 1024 * 1024;
    benchmark_config.drain_timeout_ms = 30000;
    ok &= expect(benchmark->start(benchmark_config, &error), "benchmark recorder starts");
    std::chrono::steady_clock::time_point enabled_begin = std::chrono::steady_clock::now();
    for (int i = 0; i < benchmark_records; ++i) benchmark->try_record(make_record(i + 1));
    benchmark->stop(RecorderStopMode::Drain, &error);
    const double enabled_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - enabled_begin).count();
    const RecorderDiagnostics benchmark_diagnostics = benchmark->diagnostics();
    ok &= expect(benchmark_diagnostics.written_records == benchmark_records, "benchmark records drain completely");
    ok &= expect(benchmark_diagnostics.max_enqueue_ms < 10.0, "benchmark enqueue max is bounded");
    std::cout << "recorder benchmark: records=" << benchmark_records << " disabled_submit_ms=" << disabled_ms
              << " enabled_total_ms=" << enabled_ms << " max_enqueue_ms=" << benchmark_diagnostics.max_enqueue_ms
              << " bytes=" << benchmark_diagnostics.bytes_written << std::endl;
    return ok ? 0 : 1;
}
