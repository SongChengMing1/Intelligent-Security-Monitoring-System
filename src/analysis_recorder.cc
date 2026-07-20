#include "analysis_recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "analysis_record_json.h"

namespace
{
using nlohmann::json;

long long epoch_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool make_directory(const std::string &path)
{
    if (path.empty()) return false;
    std::string current;
    for (size_t i = 0; i < path.size(); ++i)
    {
        current.push_back(path[i]);
        if (path[i] == '/' && current.size() > 1) mkdir(current.c_str(), 0755);
    }
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool atomic_write(const std::string &path, const std::string &content, std::string *error)
{
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) { if (error) *error = "cannot open " + temporary; return false; }
    output << content;
    output.flush();
    if (!output) { if (error) *error = "cannot write " + temporary; return false; }
    output.close();
    if (rename(temporary.c_str(), path.c_str()) != 0)
    { if (error) *error = "cannot rename atomic file: " + std::string(strerror(errno)); return false; }
    return true;
}

std::string segment_name(int number)
{
    std::ostringstream name;
    name << "records-" << std::setw(6) << std::setfill('0') << number << ".jsonl";
    return name.str();
}

class AnalysisRecorder : public IAnalysisRecorder
{
public:
    AnalysisRecorder() : mutex_(), cv_(), queue_(), config_(), diagnostics_(), worker_(), segment_first_ids_(), segment_last_ids_(), segment_counts_(), segment_sizes_(), queued_images_(0), image_sequence_(0), stop_requested_(false), drain_(true), started_ms_(0), drain_deadline_ms_(0), lock_fd_(-1) {}
    ~AnalysisRecorder() { std::string ignored; stop(RecorderStopMode::Immediate, &ignored); }

    bool start(const RecordingSessionConfig &config, std::string *error_message) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_.joinable()) return fail_locked("recorder is already started", error_message);
        if (config.recording_root.empty() || config.recording_session_id.empty() || config.queue_capacity_records == 0 ||
            config.segment_max_bytes == 0 || config.session_max_bytes < config.segment_max_bytes)
            return fail_locked("invalid recording session configuration", error_message);
        json resolved;
        try { resolved = json::parse(config.resolved_profile_json); }
        catch (const std::exception &exception) { return fail_locked(std::string("invalid resolved Profile JSON: ") + exception.what(), error_message); }
        config_ = config;
        diagnostics_ = RecorderDiagnostics();
        diagnostics_.state = RecorderState::Starting;
        diagnostics_.recording_session_id = config.recording_session_id;
        diagnostics_.queue_capacity = config.queue_capacity_records;
        diagnostics_.session_path = config.recording_root + "/" + config.recording_session_id;
        if (!make_directory(config.recording_root) || mkdir(diagnostics_.session_path.c_str(), 0755) != 0)
            return fail_locked("recording session directory already exists or cannot be created", error_message);
        if (config.frame_mode != "none" && !make_directory(diagnostics_.session_path + "/frames"))
            return fail_locked("cannot create recording frames directory", error_message);
        const std::string lock_path = diagnostics_.session_path + "/.writer.lock";
        lock_fd_ = open(lock_path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (lock_fd_ < 0) return fail_locked("cannot acquire session writer lock", error_message);
        started_ms_ = epoch_ms();
        json manifest = {{"manifest_schema_version", kRecordingManifestSchemaVersion}, {"record_schema_version", kAnalysisRecordSchemaVersion},
                         {"recording_session_id", config.recording_session_id}, {"runtime_session_id", config.runtime_session_id},
                         {"git_commit", config.git_commit}, {"model_path", config.model_path}, {"model_hash", config.model_hash},
                         {"labels_hash", config.labels_hash}, {"effective_profile_id", config.effective_profile_id},
                         {"effective_profile_hash", config.effective_profile_hash}, {"resolved_profile", resolved},
                         {"source_width", config.source_width}, {"source_height", config.source_height},
                         {"preprocess_mode", config.preprocess_mode}, {"started_at_ms", started_ms_}, {"frame_mode", config.frame_mode},
                         {"jpeg_every_n", config.jpeg_every_n}, {"jpeg_quality", config.jpeg_quality}};
        std::string error;
        if (!atomic_write(diagnostics_.session_path + "/manifest.json", manifest.dump(2) + "\n", &error))
            return fail_locked(error, error_message);
        stop_requested_ = false; drain_ = true; diagnostics_.state = RecorderState::Recording;
        write_status_locked();
        worker_ = std::thread(&AnalysisRecorder::run, this);
        if (error_message) error_message->clear();
        return true;
    }

    RecordEnqueueResult try_record(AnalysisRecord &&record) override
    {
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        if (diagnostics_.state == RecorderState::Disabled) return RecordEnqueueResult::Disabled;
        if (diagnostics_.state != RecorderState::Recording) return RecordEnqueueResult::Stopped;
        if (queue_.size() >= config_.queue_capacity_records)
        { ++diagnostics_.dropped_records; return RecordEnqueueResult::QueueFull; }
        if (record.image_payload)
        {
            const bool requested = config_.frame_mode == "all" ||
                                   (config_.frame_mode == "sampled" && config_.jpeg_every_n > 0 &&
                                    record.capture_frame_id % config_.jpeg_every_n == 0);
            if (!requested || config_.image_pool_capacity == 0 || queued_images_ >= config_.image_pool_capacity)
            {
                record.image_payload.reset(); record.image.state = requested ? RecordedImageState::Dropped : RecordedImageState::None;
                if (requested) ++diagnostics_.dropped_images;
            }
            else ++queued_images_;
        }
        record.timings.recorder_enqueue_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        queue_.push_back(std::move(record));
        diagnostics_.queue_size = queue_.size();
        diagnostics_.last_enqueue_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        diagnostics_.max_enqueue_ms = std::max(diagnostics_.max_enqueue_ms, diagnostics_.last_enqueue_ms);
        cv_.notify_one();
        return RecordEnqueueResult::Accepted;
    }

    bool stop(RecorderStopMode mode, std::string *error_message) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) { if (error_message) error_message->clear(); return true; }
            const bool terminal = diagnostics_.state == RecorderState::Finalized || diagnostics_.state == RecorderState::LimitReached || diagnostics_.state == RecorderState::Failed;
            if (!terminal)
            {
                stop_requested_ = true; drain_ = mode == RecorderStopMode::Drain; drain_deadline_ms_ = epoch_ms() + config_.drain_timeout_ms; diagnostics_.state = RecorderState::Finalizing;
                if (!drain_) { diagnostics_.shutdown_dropped_records += queue_.size(); diagnostics_.dropped_records += queue_.size(); queue_.clear(); diagnostics_.queue_size = 0; }
                cv_.notify_all();
            }
        }
        worker_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (error_message) *error_message = diagnostics_.error;
        return diagnostics_.state == RecorderState::Finalized || diagnostics_.state == RecorderState::LimitReached;
    }

    RecorderDiagnostics diagnostics() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RecorderDiagnostics value = diagnostics_; value.queue_size = queue_.size(); return value;
    }

private:
    bool fail_locked(const std::string &message, std::string *error_message)
    {
        diagnostics_.state = RecorderState::Failed; diagnostics_.error = message;
        if (lock_fd_ >= 0) { close(lock_fd_); lock_fd_ = -1; unlink((diagnostics_.session_path + "/.writer.lock").c_str()); }
        if (error_message) *error_message = message; return false;
    }

    size_t free_bytes() const
    {
        struct statvfs info;
        if (statvfs(diagnostics_.session_path.c_str(), &info) != 0) return 0;
        return static_cast<size_t>(info.f_bavail) * static_cast<size_t>(info.f_frsize);
    }

    void write_status_locked()
    {
        json segments = json::array();
        for (size_t i = 0; i < segment_counts_.size(); ++i)
            segments.push_back({{"number", i + 1}, {"first_frame_id", segment_first_ids_[i]}, {"last_frame_id", segment_last_ids_[i]},
                                {"record_count", segment_counts_[i]}, {"bytes", segment_sizes_[i]}});
        json status = {{"status_schema_version", kRecordingSessionStatusSchemaVersion}, {"state", recorder_state_to_string(diagnostics_.state)},
                       {"recording_session_id", diagnostics_.recording_session_id}, {"written_records", diagnostics_.written_records},
                       {"dropped_records", diagnostics_.dropped_records}, {"dropped_images", diagnostics_.dropped_images}, {"shutdown_dropped_records", diagnostics_.shutdown_dropped_records},
                       {"bytes_written", diagnostics_.bytes_written}, {"current_segment", diagnostics_.current_segment},
                       {"error", diagnostics_.error}, {"segments", segments}};
        std::string ignored; atomic_write(diagnostics_.session_path + "/session-status.json", status.dump(2) + "\n", &ignored);
    }

    void run()
    {
        std::ofstream segment;
        size_t segment_bytes = 0;
        while (true)
        {
            AnalysisRecord record;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&]() { return stop_requested_ || !queue_.empty(); });
                if (stop_requested_ && drain_ && epoch_ms() >= drain_deadline_ms_ && !queue_.empty())
                { diagnostics_.shutdown_dropped_records += queue_.size(); diagnostics_.dropped_records += queue_.size(); queue_.clear(); diagnostics_.queue_size = 0; break; }
                if (stop_requested_ && (!drain_ || queue_.empty())) break;
                record = std::move(queue_.front()); queue_.pop_front(); if (record.image_payload && queued_images_ > 0) --queued_images_; diagnostics_.queue_size = queue_.size();
            }
            if (record.image_payload)
            {
                std::vector<unsigned char> jpeg; std::string image_error;
                if (!config_.image_encoder || !config_.image_encoder(*record.image_payload, &jpeg, &image_error) || jpeg.empty())
                {
                    std::lock_guard<std::mutex> lock(mutex_); record.image_payload.reset(); record.image.state = RecordedImageState::Unavailable; ++diagnostics_.dropped_images;
                    if (!image_error.empty()) diagnostics_.error = image_error;
                }
                else
                {
                    std::ostringstream image_name; image_name << "frames/" << std::setw(8) << std::setfill('0') << ++image_sequence_ << ".jpg";
                    std::ofstream image_file((diagnostics_.session_path + "/" + image_name.str()).c_str(), std::ios::binary);
                    image_file.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size())); image_file.flush();
                    if (!image_file) { std::lock_guard<std::mutex> lock(mutex_); record.image.state = RecordedImageState::Unavailable; ++diagnostics_.dropped_images; }
                    else { record.image.state = RecordedImageState::Exact; record.image.relative_path = image_name.str(); record.image.capture_frame_id = record.capture_frame_id; }
                }
                record.image_payload.reset();
            }
            const std::string line = analysis_record_to_json(record) + "\n";
            bool limit = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool duration_limit = config_.session_max_duration_s > 0 && epoch_ms() - started_ms_ >= config_.session_max_duration_s * 1000LL;
                limit = duration_limit || diagnostics_.bytes_written + line.size() > config_.session_max_bytes || free_bytes() < config_.min_free_bytes;
                if (limit) { ++diagnostics_.dropped_records; diagnostics_.state = RecorderState::LimitReached; stop_requested_ = true; drain_ = false; diagnostics_.dropped_records += queue_.size(); queue_.clear(); }
            }
            if (limit) break;
            if (!segment.is_open() || segment_bytes + line.size() > config_.segment_max_bytes)
            {
                if (segment.is_open()) { segment.flush(); segment.close(); }
                std::lock_guard<std::mutex> lock(mutex_); ++diagnostics_.current_segment;
                segment.open((diagnostics_.session_path + "/" + segment_name(diagnostics_.current_segment)).c_str(), std::ios::binary | std::ios::app);
                segment_first_ids_.push_back(0); segment_last_ids_.push_back(0); segment_counts_.push_back(0); segment_sizes_.push_back(0);
                segment_bytes = 0;
            }
            segment << line; segment.flush();
            if (!segment)
            {
                std::lock_guard<std::mutex> lock(mutex_); diagnostics_.state = RecorderState::Failed; diagnostics_.error = "record segment write failed"; stop_requested_ = true; drain_ = false; diagnostics_.dropped_records += queue_.size(); queue_.clear(); break;
            }
            segment_bytes += line.size();
            std::lock_guard<std::mutex> lock(mutex_); ++diagnostics_.written_records; diagnostics_.bytes_written += line.size();
            const size_t index = static_cast<size_t>(diagnostics_.current_segment - 1);
            if (segment_counts_[index] == 0) segment_first_ids_[index] = record.capture_frame_id;
            segment_last_ids_[index] = record.capture_frame_id; ++segment_counts_[index]; segment_sizes_[index] += line.size(); write_status_locked();
        }
        if (segment.is_open()) { segment.flush(); segment.close(); }
        std::lock_guard<std::mutex> lock(mutex_);
        if (diagnostics_.state != RecorderState::Failed && diagnostics_.state != RecorderState::LimitReached) diagnostics_.state = RecorderState::Finalized;
        diagnostics_.queue_size = queue_.size(); write_status_locked();
        if (lock_fd_ >= 0) { close(lock_fd_); lock_fd_ = -1; unlink((diagnostics_.session_path + "/.writer.lock").c_str()); }
    }

    mutable std::mutex mutex_; std::condition_variable cv_; std::deque<AnalysisRecord> queue_;
    RecordingSessionConfig config_; RecorderDiagnostics diagnostics_; std::thread worker_;
    std::vector<long long> segment_first_ids_; std::vector<long long> segment_last_ids_;
    std::vector<long long> segment_counts_; std::vector<size_t> segment_sizes_;
    size_t queued_images_; long long image_sequence_;
    bool stop_requested_; bool drain_; long long started_ms_; long long drain_deadline_ms_; int lock_fd_;
};
} // namespace

RecordingSessionConfig::RecordingSessionConfig()
    : recording_root("recordings"), recording_session_id(), runtime_session_id(), git_commit(), model_path(), model_hash(), labels_hash(),
      effective_profile_id(), effective_profile_hash(), resolved_profile_json("{}"), source_width(0), source_height(0), preprocess_mode(),
      queue_capacity_records(128), segment_max_bytes(16U * 1024U * 1024U), session_max_bytes(1024U * 1024U * 1024U),
      session_max_duration_s(3600), min_free_bytes(256U * 1024U * 1024U), drain_timeout_ms(3000),
      frame_mode("none"), jpeg_every_n(10), jpeg_quality(80), image_pool_capacity(4), image_encoder() {}

RecorderDiagnostics::RecorderDiagnostics()
    : state(RecorderState::Disabled), recording_session_id(), session_path(), queue_size(0), queue_capacity(0), written_records(0),
      dropped_records(0), dropped_images(0), shutdown_dropped_records(0), bytes_written(0), current_segment(0), last_enqueue_ms(0.0), max_enqueue_ms(0.0), error() {}

const char *recorder_state_to_string(RecorderState state)
{
    switch (state) { case RecorderState::Disabled: return "disabled"; case RecorderState::Starting: return "starting"; case RecorderState::Recording: return "recording";
    case RecorderState::Finalizing: return "finalizing"; case RecorderState::Finalized: return "finalized"; case RecorderState::LimitReached: return "limit_reached";
    case RecorderState::Failed: return "failed"; case RecorderState::Interrupted: return "interrupted"; } return "unknown";
}

std::unique_ptr<IAnalysisRecorder> create_analysis_recorder() { return std::unique_ptr<IAnalysisRecorder>(new AnalysisRecorder()); }

bool read_analysis_session(const std::string &session_path, SessionReadResult *result)
{
    if (!result) return false; *result = SessionReadResult();
    try
    {
        std::ifstream manifest_input((session_path + "/manifest.json").c_str()); json manifest; manifest_input >> manifest;
        if (manifest.at("manifest_schema_version").get<int>() != kRecordingManifestSchemaVersion ||
            manifest.at("record_schema_version").get<int>() != kAnalysisRecordSchemaVersion)
        { result->error = "unsupported recording manifest schema"; return false; }
    }
    catch (const std::exception &exception) { result->error = std::string("invalid recording manifest: ") + exception.what(); return false; }
    for (int number = 1;; ++number)
    {
        const std::string path = session_path + "/" + segment_name(number); std::ifstream input(path.c_str(), std::ios::binary);
        if (!input) break; std::ostringstream content; content << input.rdbuf(); const std::string text = content.str();
        size_t begin = 0; while (begin < text.size())
        {
            const size_t end = text.find('\n', begin);
            if (end == std::string::npos) {
                std::ifstream next((session_path + "/" + segment_name(number + 1)).c_str());
                if (next) { result->error = path + ": truncated line before final segment"; return false; }
                result->ignored_truncated_final_line = true; break; }
            const std::string line = text.substr(begin, end - begin); begin = end + 1; if (line.empty()) continue;
            AnalysisRecord record; std::string error;
            if (!analysis_record_from_json(line, &record, &error)) { result->error = path + ": " + error; return false; }
            result->records.push_back(record);
        }
    }
    if (result->records.empty() && !result->ignored_truncated_final_line) { result->error = "session contains no complete records"; return false; }
    return true;
}

bool mark_incomplete_session_interrupted(const std::string &session_path, std::string *error_message)
{
    const std::string path = session_path + "/session-status.json"; std::ifstream input(path.c_str());
    if (!input) { if (error_message) *error_message = "session status is missing"; return false; }
    try { json status; input >> status; const std::string state = status.at("state").get<std::string>();
        if (state == "finalized" || state == "limit_reached" || state == "failed" || state == "interrupted") return true;
        status["state"] = "interrupted"; std::string error; if (!atomic_write(path, status.dump(2) + "\n", &error)) throw std::runtime_error(error);
        unlink((session_path + "/.writer.lock").c_str()); if (error_message) error_message->clear(); return true;
    } catch (const std::exception &exception) { if (error_message) *error_message = exception.what(); return false; }
}
