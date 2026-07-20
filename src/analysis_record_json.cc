#include "analysis_record_json.h"

#include <nlohmann/json.hpp>

namespace
{
using nlohmann::json;

ObservationOrigin parse_origin(const std::string &value)
{
    if (value == "high_pass") return ObservationOrigin::HighPass;
    if (value == "low_only") return ObservationOrigin::LowOnly;
    throw std::runtime_error("unsupported observation origin: " + value);
}

ObservationRejectionReason parse_reason(const std::string &value)
{
    if (value == "class_not_allowed") return ObservationRejectionReason::ClassNotAllowed;
    if (value == "below_min_width") return ObservationRejectionReason::BelowMinWidth;
    if (value == "below_min_height") return ObservationRejectionReason::BelowMinHeight;
    if (value == "below_min_area") return ObservationRejectionReason::BelowMinArea;
    if (value == "outside_roi") return ObservationRejectionReason::OutsideRoi;
    if (value == "inside_edge_margin") return ObservationRejectionReason::InsideEdgeMargin;
    if (value == "capacity_limit") return ObservationRejectionReason::CapacityLimit;
    if (value == "invalid_bbox") return ObservationRejectionReason::InvalidBBox;
    if (value == "below_tracker_low_threshold") return ObservationRejectionReason::BelowTrackerLowThreshold;
    throw std::runtime_error("unsupported rejection reason: " + value);
}

RecordedImageState parse_image_state(const std::string &value)
{
    if (value == "none") return RecordedImageState::None;
    if (value == "exact") return RecordedImageState::Exact;
    if (value == "unavailable") return RecordedImageState::Unavailable;
    if (value == "dropped") return RecordedImageState::Dropped;
    throw std::runtime_error("unsupported image state: " + value);
}

json track_to_json(const TrackObject &track)
{
    return {{"track_id", track.track_id}, {"class_id", track.class_id}, {"class_name", track.class_name},
            {"state", track_lifecycle_to_string(track.state)}, {"bbox_source", tracker_bbox_source_to_string(track.bbox_source)},
            {"score", track.score}, {"bbox", {track.bbox.x1, track.bbox.y1, track.bbox.x2, track.bbox.y2}},
            {"track_age", track.track_age}, {"hit_count", track.hit_count}, {"missed_updates", track.missed_updates}};
}

TrackLifecycle parse_lifecycle(const std::string &value)
{
    if (value == "tentative") return TrackLifecycle::Tentative;
    if (value == "confirmed") return TrackLifecycle::Confirmed;
    if (value == "lost") return TrackLifecycle::Lost;
    return TrackLifecycle::Removed;
}
} // namespace

std::string analysis_record_to_json(const AnalysisRecord &record)
{
    json observations = json::array();
    for (size_t i = 0; i < record.observations.size(); ++i)
    {
        const AnalysisObservation &value = record.observations[i];
        json reasons = json::array();
        for (size_t j = 0; j < value.rejection_reasons.size(); ++j)
            reasons.push_back(observation_rejection_reason_to_string(value.rejection_reasons[j]));
        observations.push_back({{"observation_id", value.observation_id}, {"origin", observation_origin_to_string(value.origin)},
                                {"class_id", value.class_id}, {"class_name", value.class_name}, {"score", value.score},
                                {"bbox", {value.bbox.x1, value.bbox.y1, value.bbox.x2, value.bbox.y2}},
                                {"admitted", value.admitted}, {"rejection_reasons", reasons}});
    }
    json tracks = json::array();
    for (size_t i = 0; i < record.recorded_tracks.objects.size(); ++i)
        tracks.push_back(track_to_json(record.recorded_tracks.objects[i]));
    json value = {
        {"record_schema_version", record.record_schema_version}, {"recording_session_id", record.recording_session_id},
        {"runtime_session_id", record.runtime_session_id}, {"capture_frame_id", record.capture_frame_id},
        {"capture_timestamp_ms", record.capture_timestamp_ms}, {"record_timestamp_ms", record.record_timestamp_ms},
        {"source_width", record.source_width}, {"source_height", record.source_height}, {"observations", observations},
        {"tracker_input_observation_ids", record.tracker_input_observation_ids},
        {"recorded_tracks", {{"capture_frame_id", record.recorded_tracks.capture_frame_id},
                             {"capture_timestamp_ms", record.recorded_tracks.capture_timestamp_ms}, {"objects", tracks}}},
        {"effective_profile_id", record.effective_profile_id}, {"effective_profile_hash", record.effective_profile_hash},
        {"timings", {{"preprocess_ms", record.timings.preprocess_ms}, {"inference_ms", record.timings.inference_ms},
                     {"postprocess_ms", record.timings.postprocess_ms}, {"policy_ms", record.timings.policy_ms},
                     {"tracker_ms", record.timings.tracker_ms}, {"recorder_enqueue_ms", record.timings.recorder_enqueue_ms}}},
        {"image", {{"state", recorded_image_state_to_string(record.image.state)},
                   {"relative_path", record.image.relative_path}, {"capture_frame_id", record.image.capture_frame_id}}}};
    return value.dump();
}

bool analysis_record_from_json(const std::string &json_text, AnalysisRecord *record, std::string *error_message)
{
    if (record == NULL) return false;
    try
    {
        const json value = json::parse(json_text);
        AnalysisRecord output;
        output.record_schema_version = value.at("record_schema_version").get<int>();
        if (output.record_schema_version != kAnalysisRecordSchemaVersion)
            throw std::runtime_error("unsupported record_schema_version");
        output.recording_session_id = value.at("recording_session_id").get<std::string>();
        output.runtime_session_id = value.at("runtime_session_id").get<std::string>();
        output.capture_frame_id = value.at("capture_frame_id").get<long long>();
        output.capture_timestamp_ms = value.at("capture_timestamp_ms").get<long long>();
        output.record_timestamp_ms = value.at("record_timestamp_ms").get<long long>();
        output.source_width = value.at("source_width").get<int>();
        output.source_height = value.at("source_height").get<int>();
        const json &observations = value.at("observations");
        for (size_t i = 0; i < observations.size(); ++i)
        {
            AnalysisObservation observation;
            observation.observation_id = observations[i].at("observation_id").get<uint32_t>();
            observation.origin = parse_origin(observations[i].at("origin").get<std::string>());
            observation.class_id = observations[i].at("class_id").get<int>();
            observation.class_name = observations[i].at("class_name").get<std::string>();
            observation.score = observations[i].at("score").get<float>();
            const json &bbox = observations[i].at("bbox");
            observation.bbox.x1 = bbox[0].get<float>(); observation.bbox.y1 = bbox[1].get<float>();
            observation.bbox.x2 = bbox[2].get<float>(); observation.bbox.y2 = bbox[3].get<float>();
            observation.admitted = observations[i].at("admitted").get<bool>();
            const json &reasons = observations[i].at("rejection_reasons");
            for (size_t j = 0; j < reasons.size(); ++j) observation.rejection_reasons.push_back(parse_reason(reasons[j].get<std::string>()));
            output.observations.push_back(observation);
        }
        output.tracker_input_observation_ids = value.at("tracker_input_observation_ids").get<std::vector<uint32_t> >();
        const json &track_frame = value.at("recorded_tracks");
        output.recorded_tracks.capture_frame_id = track_frame.at("capture_frame_id").get<long long>();
        output.recorded_tracks.capture_timestamp_ms = track_frame.at("capture_timestamp_ms").get<long long>();
        output.recorded_tracks.source_width = output.source_width; output.recorded_tracks.source_height = output.source_height;
        const json &tracks = track_frame.at("objects");
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            TrackObject track;
            track.track_id = tracks[i].at("track_id").get<uint64_t>(); track.class_id = tracks[i].at("class_id").get<int>();
            track.class_name = tracks[i].at("class_name").get<std::string>(); track.state = parse_lifecycle(tracks[i].at("state").get<std::string>());
            track.bbox_source = tracks[i].at("bbox_source").get<std::string>() == "predicted" ? TrackerBBoxSource::Predicted : TrackerBBoxSource::Observed;
            track.score = tracks[i].at("score").get<float>(); const json &bbox = tracks[i].at("bbox");
            track.bbox.x1 = bbox[0].get<float>(); track.bbox.y1 = bbox[1].get<float>(); track.bbox.x2 = bbox[2].get<float>(); track.bbox.y2 = bbox[3].get<float>();
            track.track_age = tracks[i].at("track_age").get<int>(); track.hit_count = tracks[i].at("hit_count").get<int>(); track.missed_updates = tracks[i].at("missed_updates").get<int>();
            output.recorded_tracks.objects.push_back(track);
        }
        output.effective_profile_id = value.at("effective_profile_id").get<std::string>();
        output.effective_profile_hash = value.at("effective_profile_hash").get<std::string>();
        const json &timings = value.at("timings");
        output.timings.preprocess_ms = timings.at("preprocess_ms").get<double>(); output.timings.inference_ms = timings.at("inference_ms").get<double>();
        output.timings.postprocess_ms = timings.at("postprocess_ms").get<double>(); output.timings.policy_ms = timings.at("policy_ms").get<double>();
        output.timings.tracker_ms = timings.at("tracker_ms").get<double>(); output.timings.recorder_enqueue_ms = timings.at("recorder_enqueue_ms").get<double>();
        const json &image = value.at("image"); output.image.state = parse_image_state(image.at("state").get<std::string>());
        output.image.relative_path = image.value("relative_path", ""); output.image.capture_frame_id = image.value("capture_frame_id", 0LL);
        *record = output;
        if (error_message) error_message->clear();
        return true;
    }
    catch (const std::exception &exception)
    {
        if (error_message) *error_message = exception.what();
        return false;
    }
}
