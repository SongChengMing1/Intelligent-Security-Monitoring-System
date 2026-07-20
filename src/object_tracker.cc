#include "object_tracker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <utility>

#include "BYTETracker.h"

namespace
{
struct PublicTrackRecord
{
    uint64_t public_id;
    int class_id;
    std::string class_name;
    TrackLifecycle state;
    float score;
    TrackerBBox bbox;
    int age;
    int hits;
    int missed_updates;
    long long last_seen_timestamp_ms;

    PublicTrackRecord()
        : public_id(0),
          class_id(-1),
          class_name(),
          state(TrackLifecycle::Tentative),
          score(0.0f),
          bbox(),
          age(0),
          hits(0),
          missed_updates(0),
          last_seen_timestamp_ms(0)
    {
    }
};

typedef std::pair<int, int> InternalTrackKey;

class ByteTrackObjectTracker : public IObjectTracker
{
public:
    explicit ByteTrackObjectTracker(const TrackerConfig &config)
        : config_(config),
          class_trackers_(),
          class_names_(),
          records_(),
          next_public_id_(1),
          last_frame_id_(0),
          last_timestamp_ms_(0),
          source_width_(0),
          source_height_(0),
          diagnostics_(),
          total_update_ms_(0.0)
    {
    }

    bool update(const DetectionFrame &detections, TrackFrame *tracks, std::string *error_message) override
    {
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        if (tracks == NULL)
        {
            return fail("track output is null", error_message, begin);
        }
        tracks->objects.clear();
        tracks->capture_frame_id = detections.capture_frame_id;
        tracks->capture_timestamp_ms = detections.capture_timestamp_ms;
        tracks->source_width = detections.source_width;
        tracks->source_height = detections.source_height;

        if (detections.capture_frame_id <= 0 || detections.capture_timestamp_ms <= 0 ||
            detections.source_width <= 0 || detections.source_height <= 0)
        {
            return fail("invalid detection frame metadata", error_message, begin);
        }

        int steps = 1;
        if (last_timestamp_ms_ > 0)
        {
            const long long delta_ms = detections.capture_timestamp_ms - last_timestamp_ms_;
            const bool dimensions_changed = source_width_ != detections.source_width ||
                                            source_height_ != detections.source_height;
            if (delta_ms <= 0 || delta_ms > config_.max_timestamp_gap_ms || dimensions_changed)
            {
                reset_state(true);
            }
            else
            {
                steps = std::max(1, static_cast<int>((delta_ms + config_.nominal_interval_ms / 2) /
                                                     config_.nominal_interval_ms));
            }
        }

        std::vector<DetectionObject> candidates;
        for (size_t i = 0; i < detections.objects.size(); ++i)
        {
            const DetectionObject &object = detections.objects[i];
            if (object.class_id < 0 || object.score < config_.low_threshold || object.score > 1.0f ||
                object.bbox.x2 <= object.bbox.x1 || object.bbox.y2 <= object.bbox.y1)
            {
                diagnostics_.dropped_observations++;
                continue;
            }
            candidates.push_back(object);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const DetectionObject &left,
                                                                  const DetectionObject &right) {
            return left.score > right.score;
        });
        if (candidates.size() > config_.max_tracks)
        {
            diagnostics_.dropped_observations += candidates.size() - config_.max_tracks;
            candidates.resize(config_.max_tracks);
        }

        try
        {
            for (int step = 1; step < steps; ++step)
            {
                const long long timestamp_ms = last_timestamp_ms_ +
                                               static_cast<long long>(step) * config_.nominal_interval_ms;
                advance(std::map<int, std::vector<Object> >(), timestamp_ms, NULL);
            }

            std::map<int, std::vector<Object> > grouped;
            for (size_t i = 0; i < candidates.size(); ++i)
            {
                const DetectionObject &candidate = candidates[i];
                Object object;
                object.x = candidate.bbox.x1;
                object.y = candidate.bbox.y1;
                object.width = candidate.bbox.x2 - candidate.bbox.x1;
                object.height = candidate.bbox.y2 - candidate.bbox.y1;
                object.prob = candidate.score;
                grouped[candidate.class_id].push_back(object);
                class_names_[candidate.class_id] = candidate.class_name;
            }
            advance(grouped, detections.capture_timestamp_ms, tracks);
        }
        catch (const std::exception &exception)
        {
            return fail(exception.what(), error_message, begin);
        }

        last_frame_id_ = detections.capture_frame_id;
        last_timestamp_ms_ = detections.capture_timestamp_ms;
        source_width_ = detections.source_width;
        source_height_ = detections.source_height;
        finish_timing(begin);
        if (error_message != NULL)
        {
            error_message->clear();
        }
        return true;
    }

    void reset() override
    {
        reset_state(true);
    }

    TrackerDiagnostics diagnostics() const override
    {
        return diagnostics_;
    }

private:
    std::unique_ptr<BYTETracker> make_core() const
    {
        const int max_lost_steps = std::max(1, (config_.lost_timeout_ms + config_.nominal_interval_ms - 1) /
                                                   config_.nominal_interval_ms);
        return std::unique_ptr<BYTETracker>(new BYTETracker(config_.low_threshold,
                                                            config_.high_threshold,
                                                            config_.new_track_threshold,
                                                            config_.match_threshold,
                                                            config_.second_match_threshold,
                                                            max_lost_steps));
    }

    void advance(const std::map<int, std::vector<Object> > &grouped,
                 long long timestamp_ms,
                 TrackFrame *output)
    {
        std::set<int> class_ids;
        for (std::map<int, std::unique_ptr<BYTETracker> >::const_iterator it = class_trackers_.begin();
             it != class_trackers_.end(); ++it)
        {
            class_ids.insert(it->first);
        }
        for (std::map<int, std::vector<Object> >::const_iterator it = grouped.begin(); it != grouped.end(); ++it)
        {
            class_ids.insert(it->first);
            if (class_trackers_.count(it->first) == 0)
            {
                class_trackers_[it->first] = make_core();
            }
        }

        for (std::map<InternalTrackKey, PublicTrackRecord>::iterator it = records_.begin();
             it != records_.end(); ++it)
        {
            it->second.age++;
            it->second.missed_updates++;
            it->second.state = TrackLifecycle::Lost;
        }

        std::set<InternalTrackKey> active_keys;
        for (std::set<int>::const_iterator class_it = class_ids.begin(); class_it != class_ids.end(); ++class_it)
        {
            const int class_id = *class_it;
            const std::map<int, std::vector<Object> >::const_iterator objects_it = grouped.find(class_id);
            const std::vector<Object> empty;
            const std::vector<Object> &objects = objects_it == grouped.end() ? empty : objects_it->second;
            const std::vector<STrack> active = class_trackers_[class_id]->update(objects);

            for (size_t i = 0; i < active.size(); ++i)
            {
                const STrack &track = active[i];
                const InternalTrackKey key(class_id, track.track_id);
                PublicTrackRecord &record = records_[key];
                if (record.public_id == 0)
                {
                    record.public_id = next_public_id_++;
                    record.class_id = class_id;
                    record.class_name = class_names_[class_id];
                    record.age = 0;
                }
                record.class_name = class_names_[class_id];
                record.hits++;
                record.missed_updates = 0;
                record.last_seen_timestamp_ms = timestamp_ms;
                record.state = record.hits >= config_.confirm_hits ? TrackLifecycle::Confirmed
                                                                   : TrackLifecycle::Tentative;
                record.score = track.score;
                record.bbox.x1 = track.tlbr[0];
                record.bbox.y1 = track.tlbr[1];
                record.bbox.x2 = track.tlbr[2];
                record.bbox.y2 = track.tlbr[3];
                active_keys.insert(key);
            }
        }

        for (std::map<InternalTrackKey, PublicTrackRecord>::iterator it = records_.begin();
             it != records_.end();)
        {
            PublicTrackRecord &record = it->second;
            const bool expired = timestamp_ms - record.last_seen_timestamp_ms > config_.lost_timeout_ms;
            const bool unconfirmed_missed = record.hits < config_.confirm_hits && record.missed_updates > 0;
            if (expired || unconfirmed_missed)
            {
                it = records_.erase(it);
                continue;
            }
            ++it;
        }

        update_state_counts();
        if (output == NULL)
        {
            return;
        }
        for (std::set<InternalTrackKey>::const_iterator it = active_keys.begin(); it != active_keys.end(); ++it)
        {
            const PublicTrackRecord &record = records_.find(*it)->second;
            TrackObject object;
            object.track_id = record.public_id;
            object.class_id = record.class_id;
            object.class_name = record.class_name;
            object.state = record.state;
            object.bbox_source = TrackerBBoxSource::Observed;
            object.score = record.score;
            object.bbox = record.bbox;
            object.track_age = record.age;
            object.hit_count = record.hits;
            object.missed_updates = record.missed_updates;
            output->objects.push_back(object);
        }
    }

    void update_state_counts()
    {
        diagnostics_.tentative_count = 0;
        diagnostics_.confirmed_count = 0;
        diagnostics_.lost_count = 0;
        for (std::map<InternalTrackKey, PublicTrackRecord>::const_iterator it = records_.begin();
             it != records_.end(); ++it)
        {
            if (it->second.state == TrackLifecycle::Tentative)
            {
                diagnostics_.tentative_count++;
            }
            else if (it->second.state == TrackLifecycle::Confirmed)
            {
                diagnostics_.confirmed_count++;
            }
            else if (it->second.state == TrackLifecycle::Lost)
            {
                diagnostics_.lost_count++;
            }
        }
    }

    void reset_state(bool count_reset)
    {
        class_trackers_.clear();
        class_names_.clear();
        records_.clear();
        next_public_id_ = 1;
        last_frame_id_ = 0;
        last_timestamp_ms_ = 0;
        source_width_ = 0;
        source_height_ = 0;
        diagnostics_.tentative_count = 0;
        diagnostics_.confirmed_count = 0;
        diagnostics_.lost_count = 0;
        if (count_reset)
        {
            diagnostics_.reset_count++;
        }
    }

    bool fail(const std::string &message,
              std::string *error_message,
              const std::chrono::steady_clock::time_point &begin)
    {
        diagnostics_.error_count++;
        diagnostics_.last_error = message;
        if (error_message != NULL)
        {
            *error_message = message;
        }
        finish_timing(begin);
        return false;
    }

    void finish_timing(const std::chrono::steady_clock::time_point &begin)
    {
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - begin;
        diagnostics_.last_update_ms = elapsed.count();
        diagnostics_.max_update_ms = std::max(diagnostics_.max_update_ms, diagnostics_.last_update_ms);
        diagnostics_.update_count++;
        total_update_ms_ += diagnostics_.last_update_ms;
        diagnostics_.average_update_ms = total_update_ms_ / diagnostics_.update_count;
    }

    TrackerConfig config_;
    std::map<int, std::unique_ptr<BYTETracker> > class_trackers_;
    std::map<int, std::string> class_names_;
    std::map<InternalTrackKey, PublicTrackRecord> records_;
    uint64_t next_public_id_;
    long long last_frame_id_;
    long long last_timestamp_ms_;
    int source_width_;
    int source_height_;
    TrackerDiagnostics diagnostics_;
    double total_update_ms_;
};
} // namespace

const char *track_lifecycle_to_string(TrackLifecycle state)
{
    switch (state)
    {
    case TrackLifecycle::Tentative:
        return "tentative";
    case TrackLifecycle::Confirmed:
        return "confirmed";
    case TrackLifecycle::Lost:
        return "lost";
    case TrackLifecycle::Removed:
        return "removed";
    }
    return "unknown";
}

const char *tracker_bbox_source_to_string(TrackerBBoxSource source)
{
    return source == TrackerBBoxSource::Predicted ? "predicted" : "observed";
}

bool validate_tracker_config(const TrackerConfig &config, std::string *error_message)
{
    std::string message;
    if (config.low_threshold < 0.0f || config.low_threshold >= config.high_threshold ||
        config.high_threshold > 1.0f)
    {
        message = "tracker thresholds must satisfy 0 <= low < high <= 1";
    }
    else if (config.new_track_threshold < config.high_threshold || config.new_track_threshold > 1.0f)
    {
        message = "new-track threshold must satisfy high <= new_track <= 1";
    }
    else if (config.match_threshold <= 0.0f || config.match_threshold > 1.0f ||
             config.second_match_threshold <= 0.0f || config.second_match_threshold > 1.0f)
    {
        message = "association thresholds must be in (0, 1]";
    }
    else if (config.confirm_hits <= 0 || config.lost_timeout_ms <= 0 ||
             config.nominal_interval_ms <= 0 || config.max_timestamp_gap_ms <= 0 ||
             config.max_tracks == 0)
    {
        message = "tracker count and time bounds must be positive";
    }

    if (error_message != NULL)
    {
        *error_message = message;
    }
    return message.empty();
}

std::unique_ptr<IObjectTracker> create_bytetrack_object_tracker(const TrackerConfig &config,
                                                                std::string *error_message)
{
    if (!validate_tracker_config(config, error_message))
    {
        return std::unique_ptr<IObjectTracker>();
    }
    if (error_message != NULL)
    {
        error_message->clear();
    }
    return std::unique_ptr<IObjectTracker>(new ByteTrackObjectTracker(config));
}
