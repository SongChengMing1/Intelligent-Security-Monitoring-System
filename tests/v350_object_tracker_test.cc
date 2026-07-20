#include "object_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct FixtureRow
{
    std::string sequence;
    long long frame_id;
    long long timestamp_ms;
    std::string object_key;
    int class_id;
    std::string class_name;
    float score;
    TrackerBBox bbox;
};

typedef std::map<long long, std::vector<FixtureRow> > SequenceFrames;

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

std::vector<std::string> split_csv(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
    {
        fields.push_back(field);
    }
    return fields;
}

std::map<std::string, SequenceFrames> load_fixtures(const std::string &path)
{
    std::ifstream stream(path.c_str());
    require(stream.good(), "failed to open fixture " + path);
    std::string line;
    std::getline(stream, line);
    require(line == "sequence,frame_id,timestamp_ms,object_key,class_id,class_name,score,x1,y1,x2,y2",
            "unexpected fixture header");

    std::map<std::string, SequenceFrames> sequences;
    while (std::getline(stream, line))
    {
        if (line.empty())
        {
            continue;
        }
        const std::vector<std::string> fields = split_csv(line);
        require(fields.size() == 11, "fixture row does not contain 11 fields");
        FixtureRow row;
        row.sequence = fields[0];
        row.frame_id = std::atoll(fields[1].c_str());
        row.timestamp_ms = std::atoll(fields[2].c_str());
        row.object_key = fields[3];
        row.class_id = std::atoi(fields[4].c_str());
        row.class_name = fields[5];
        row.score = static_cast<float>(std::atof(fields[6].c_str()));
        row.bbox.x1 = static_cast<float>(std::atof(fields[7].c_str()));
        row.bbox.y1 = static_cast<float>(std::atof(fields[8].c_str()));
        row.bbox.x2 = static_cast<float>(std::atof(fields[9].c_str()));
        row.bbox.y2 = static_cast<float>(std::atof(fields[10].c_str()));
        sequences[row.sequence][row.frame_id].push_back(row);
    }
    return sequences;
}

float iou(const TrackerBBox &left, const TrackerBBox &right)
{
    const float x1 = std::max(left.x1, right.x1);
    const float y1 = std::max(left.y1, right.y1);
    const float x2 = std::min(left.x2, right.x2);
    const float y2 = std::min(left.y2, right.y2);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float left_area = (left.x2 - left.x1) * (left.y2 - left.y1);
    const float right_area = (right.x2 - right.x1) * (right.y2 - right.y1);
    const float union_area = left_area + right_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

DetectionFrame make_frame(const std::vector<FixtureRow> &rows)
{
    DetectionFrame frame;
    frame.capture_frame_id = rows[0].frame_id;
    frame.capture_timestamp_ms = rows[0].timestamp_ms;
    frame.source_width = 640;
    frame.source_height = 480;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (rows[i].object_key == "-")
        {
            continue;
        }
        DetectionObject object;
        object.class_id = rows[i].class_id;
        object.class_name = rows[i].class_name;
        object.score = rows[i].score;
        object.bbox = rows[i].bbox;
        frame.objects.push_back(object);
    }
    return frame;
}

void verify_sequence(const std::string &name, const SequenceFrames &frames)
{
    TrackerConfig config;
    config.confirm_hits = 2;
    config.lost_timeout_ms = 1000;
    std::string error;
    std::unique_ptr<IObjectTracker> tracker = create_bytetrack_object_tracker(config, &error);
    require(tracker.get() != NULL, name + ": tracker creation failed: " + error);

    std::map<std::string, uint64_t> ground_truth_ids;
    for (SequenceFrames::const_iterator frame_it = frames.begin(); frame_it != frames.end(); ++frame_it)
    {
        const std::vector<FixtureRow> &rows = frame_it->second;
        TrackFrame tracks;
        require(tracker->update(make_frame(rows), &tracks, &error), name + ": update failed: " + error);

        std::set<size_t> used_tracks;
        for (size_t row_index = 0; row_index < rows.size(); ++row_index)
        {
            const FixtureRow &row = rows[row_index];
            if (row.object_key == "-")
            {
                continue;
            }
            float best_iou = 0.0f;
            size_t best_index = tracks.objects.size();
            for (size_t track_index = 0; track_index < tracks.objects.size(); ++track_index)
            {
                if (used_tracks.count(track_index) != 0 || tracks.objects[track_index].class_id != row.class_id)
                {
                    continue;
                }
                const float overlap = iou(row.bbox, tracks.objects[track_index].bbox);
                if (overlap > best_iou)
                {
                    best_iou = overlap;
                    best_index = track_index;
                }
            }
            if (best_index == tracks.objects.size() || best_iou < 0.10f)
            {
                continue;
            }
            used_tracks.insert(best_index);
            const uint64_t track_id = tracks.objects[best_index].track_id;
            if (ground_truth_ids.count(row.object_key) == 0)
            {
                ground_truth_ids[row.object_key] = track_id;
            }
            else
            {
                require(ground_truth_ids[row.object_key] == track_id,
                        name + ": identity changed for " + row.object_key);
            }
        }

        if (name == "short_miss" && frame_it->first == 3)
        {
            require(tracker->diagnostics().lost_count == 1, "short_miss: expected one lost track");
        }
    }

    if (name == "exit_reentry")
    {
        require(ground_truth_ids.count("person-A") == 1 && ground_truth_ids.count("person-B") == 1,
                "exit_reentry: expected both track segments");
        require(ground_truth_ids["person-A"] != ground_truth_ids["person-B"],
                "exit_reentry: re-entry after timeout reused an old ID");
    }
    require(tracker->diagnostics().error_count == 0, name + ": tracker reported errors");
    const TrackerDiagnostics diagnostics = tracker->diagnostics();
    std::cout << "SCENARIO " << name << " ids=";
    for (std::map<std::string, uint64_t>::const_iterator it = ground_truth_ids.begin();
         it != ground_truth_ids.end(); ++it)
    {
        if (it != ground_truth_ids.begin())
        {
            std::cout << ',';
        }
        std::cout << it->first << ':' << it->second;
    }
    std::cout << " updates=" << diagnostics.update_count
              << " confirmed=" << diagnostics.confirmed_count
              << " lost=" << diagnostics.lost_count
              << " errors=" << diagnostics.error_count << std::endl;
}

DetectionObject make_object(int class_id, float score, float x1)
{
    DetectionObject object;
    object.class_id = class_id;
    object.class_name = class_id == 0 ? "person" : "car";
    object.score = score;
    object.bbox.x1 = x1;
    object.bbox.y1 = 40.0f;
    object.bbox.x2 = x1 + 20.0f;
    object.bbox.y2 = 100.0f;
    return object;
}

void verify_capacity_and_reset()
{
    TrackerConfig config;
    config.max_tracks = 4;
    std::string error;
    std::unique_ptr<IObjectTracker> tracker = create_bytetrack_object_tracker(config, &error);
    require(tracker.get() != NULL, "capacity: tracker creation failed");

    TrackFrame tracks;
    for (int frame_index = 1; frame_index <= 2; ++frame_index)
    {
        DetectionFrame frame;
        frame.capture_frame_id = frame_index;
        frame.capture_timestamp_ms = frame_index * 100;
        frame.source_width = 640;
        frame.source_height = 480;
        for (int i = 0; i < 10; ++i)
        {
            frame.objects.push_back(make_object(0, 0.90f - i * 0.01f, static_cast<float>(i * 30)));
        }
        require(tracker->update(frame, &tracks, &error), "capacity: update failed");
    }
    require(tracks.objects.size() <= config.max_tracks, "capacity: output exceeded max_tracks");
    require(tracker->diagnostics().dropped_observations == 12,
            "capacity: expected six dropped observations per update");

    const uint64_t before_reset = tracks.objects.empty() ? 0 : tracks.objects[0].track_id;
    require(before_reset == 1, "reset: first run should allocate public ID 1");
    tracker->reset();
    for (int frame_index = 1; frame_index <= 2; ++frame_index)
    {
        DetectionFrame frame;
        frame.capture_frame_id = frame_index;
        frame.capture_timestamp_ms = 1000 + frame_index * 100;
        frame.source_width = 640;
        frame.source_height = 480;
        frame.objects.push_back(make_object(0, 0.90f, 20.0f + frame_index));
        require(tracker->update(frame, &tracks, &error), "reset: update failed");
    }
    require(!tracks.objects.empty() && tracks.objects[0].track_id == 1,
            "reset: public ID allocator did not restart for the new run");
    require(tracker->diagnostics().reset_count == 1, "reset: reset counter mismatch");
    std::cout << "SCENARIO capacity_reset max_tracks=" << config.max_tracks
              << " dropped=12 before_reset_id=" << before_reset
              << " after_reset_id=" << tracks.objects[0].track_id
              << " resets=" << tracker->diagnostics().reset_count << std::endl;
}

void verify_timestamp_discontinuity_and_config()
{
    TrackerConfig invalid;
    invalid.low_threshold = 0.6f;
    invalid.high_threshold = 0.5f;
    std::string error;
    require(create_bytetrack_object_tracker(invalid, &error).get() == NULL,
            "config: invalid thresholds were accepted");
    require(!error.empty(), "config: invalid thresholds did not report an error");

    TrackerConfig config;
    std::unique_ptr<IObjectTracker> tracker = create_bytetrack_object_tracker(config, &error);
    TrackFrame tracks;
    DetectionFrame frame;
    frame.capture_frame_id = 1;
    frame.capture_timestamp_ms = 200;
    frame.source_width = 640;
    frame.source_height = 480;
    frame.objects.push_back(make_object(0, 0.9f, 20.0f));
    require(tracker->update(frame, &tracks, &error), "timestamp: first update failed");
    frame.capture_frame_id = 2;
    frame.capture_timestamp_ms = 100;
    require(tracker->update(frame, &tracks, &error), "timestamp: reset update failed");
    require(tracker->diagnostics().reset_count == 1, "timestamp: backwards time did not reset tracker");
    std::cout << "SCENARIO backwards_timestamp resets=" << tracker->diagnostics().reset_count
              << " errors=" << tracker->diagnostics().error_count << std::endl;
}
} // namespace

int main(int argc, char **argv)
{
    const std::string fixture_path = argc > 1 ? argv[1] : "tests/data/v350_tracking_sequences.csv";
    const std::map<std::string, SequenceFrames> sequences = load_fixtures(fixture_path);
    require(sequences.size() == 8, "expected eight deterministic sequences");
    for (std::map<std::string, SequenceFrames>::const_iterator it = sequences.begin(); it != sequences.end(); ++it)
    {
        verify_sequence(it->first, it->second);
    }
    verify_capacity_and_reset();
    verify_timestamp_discontinuity_and_config();
    std::cout << "PASS v350_object_tracker_test sequences=" << sequences.size() << std::endl;
    return 0;
}
