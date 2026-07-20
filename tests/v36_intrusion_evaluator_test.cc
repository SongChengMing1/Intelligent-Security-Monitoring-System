#include <iostream>
#include <string>
#include <vector>

#include "intrusion_evaluator.h"

namespace
{
bool expect(bool condition, const std::string &message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

TrackObject make_person(uint64_t track_id, float x1, float y1, float x2, float y2,
                        TrackerBBoxSource bbox_source = TrackerBBoxSource::Observed)
{
    TrackObject person;
    person.track_id = track_id;
    person.class_id = 0;
    person.class_name = "person";
    person.state = TrackLifecycle::Confirmed;
    person.bbox_source = bbox_source;
    person.score = 0.95f;
    person.bbox.x1 = x1;
    person.bbox.y1 = y1;
    person.bbox.x2 = x2;
    person.bbox.y2 = y2;
    person.hit_count = 3;
    return person;
}

TrackFrame make_frame_with_tracks(long long frame_id, long long timestamp_ms,
                                  const std::vector<TrackObject> &objects)
{
    TrackFrame frame;
    frame.capture_frame_id = frame_id;
    frame.capture_timestamp_ms = timestamp_ms;
    frame.source_width = 640;
    frame.source_height = 480;
    frame.objects = objects;
    return frame;
}

TrackFrame make_frame(long long frame_id, long long timestamp_ms, float x1, float y1,
                      float x2, float y2)
{
    std::vector<TrackObject> objects;
    objects.push_back(make_person(7, x1, y1, x2, y2));
    return make_frame_with_tracks(frame_id, timestamp_ms, objects);
}

TrackFrame make_empty_frame(long long frame_id, long long timestamp_ms)
{
    std::vector<TrackObject> objects;
    return make_frame_with_tracks(frame_id, timestamp_ms, objects);
}

IntrusionRuleConfig make_config()
{
    IntrusionRuleConfig config;
    config.enabled = true;
    config.rule_id = "person-intrusion";
    config.class_ids.push_back(0);
    config.region.x1 = 0.0f;
    config.region.y1 = 0.0f;
    config.region.x2 = 0.5f;
    config.region.y2 = 1.0f;
    config.region.enabled = true;
    config.dwell_ms = 5000;
    return config;
}

bool expect_invalid_config(const IntrusionRuleConfig &config, const std::string &message)
{
    std::string error;
    return expect(!validate_intrusion_rule_config(config, &error) && !error.empty(), message);
}

bool test_config_validation_and_evaluator_failures()
{
    IntrusionRuleConfig disabled;
    disabled.enabled = false;
    if (!expect(validate_intrusion_rule_config(disabled, NULL),
                "explicit disabled rule validates without active rule fields"))
        return false;

    IntrusionRuleConfig invalid = make_config();
    invalid.rule_id.clear();
    if (!expect_invalid_config(invalid, "empty rule id is rejected")) return false;

    invalid = make_config();
    invalid.class_ids.clear();
    if (!expect_invalid_config(invalid, "empty class list is rejected")) return false;

    invalid = make_config();
    invalid.class_ids.push_back(0);
    if (!expect_invalid_config(invalid, "duplicate class id is rejected")) return false;

    invalid = make_config();
    invalid.region.enabled = false;
    if (!expect_invalid_config(invalid, "disabled region is rejected for an enabled rule")) return false;

    invalid = make_config();
    invalid.region.x1 = 0.6f;
    if (!expect_invalid_config(invalid, "inverted region is rejected")) return false;

    invalid = make_config();
    invalid.dwell_ms = 0;
    if (!expect_invalid_config(invalid, "non-positive dwell is rejected")) return false;

    invalid = make_config();
    invalid.boundary_hysteresis_px = -1.0f;
    if (!expect_invalid_config(invalid, "negative hysteresis is rejected")) return false;

    invalid = make_config();
    invalid.prediction_grace_ms = -1;
    if (!expect_invalid_config(invalid, "negative prediction grace is rejected")) return false;

    invalid = make_config();
    invalid.recent_event_capacity = 0;
    if (!expect_invalid_config(invalid, "zero recent event capacity is rejected")) return false;

    IntrusionEvaluator evaluator(make_config());
    IntrusionFrame output;
    std::string error;
    TrackFrame invalid_frame;
    if (!expect(!evaluator.update(invalid_frame, &output, &error),
                "invalid track metadata fails evaluator update") ||
        !expect(output.state == "unavailable", "invalid track metadata reports unavailable") ||
        !expect(!error.empty(), "invalid track metadata exposes an error"))
        return false;

    if (!expect(evaluator.update(make_frame(1, 1000, 100, 100, 200, 300), &output, &error),
                "evaluator recovers after invalid track metadata: " + error) ||
        !expect(output.state == "running", "evaluator recovery reports running"))
        return false;

    if (!expect(!evaluator.update(make_frame(1, 900, 100, 100, 200, 300), &output, &error),
                "non-increasing capture timestamp fails evaluator update") ||
        !expect(output.state == "unavailable", "non-increasing timestamp reports unavailable") ||
        !expect(evaluator.update(make_frame(2, 1100, 100, 100, 200, 300), &output, &error),
                "evaluator recovers after timestamp failure: " + error))
        return false;

    std::vector<TrackObject> duplicate_ids;
    duplicate_ids.push_back(make_person(7, 100, 100, 200, 300));
    duplicate_ids.push_back(make_person(7, 200, 100, 300, 300));
    if (!expect(!evaluator.update(make_frame_with_tracks(3, 1200, duplicate_ids), &output, &error),
                "duplicate track ids fail evaluator update") ||
        !expect(output.state == "unavailable", "duplicate track ids report unavailable") ||
        !expect(!error.empty(), "duplicate track ids expose an error"))
        return false;
    return true;
}

bool test_incomplete_dwell_does_not_emit_event()
{
    IntrusionEvaluator evaluator(make_config());
    IntrusionFrame output;
    std::string error;

    if (!expect(evaluator.update(make_frame(1, 1000, 100, 100, 200, 300), &output, &error),
                "incomplete dwell starts: " + error) ||
        !expect(evaluator.update(make_frame(2, 4000, 280, 100, 380, 300), &output, &error),
                "early exit evaluates: " + error))
        return false;
    if (!expect(output.in_region_targets.empty(), "early exit removes incomplete dwell") ||
        !expect(output.active_alarms.empty(), "early exit has no active alarm") ||
        !expect(output.recent_events.empty(), "early exit emits no formal event") ||
        !expect(output.event_sequence == 0, "early exit does not advance event sequence"))
        return false;

    if (!expect(evaluator.update(make_frame(3, 5000, 100, 100, 200, 300), &output, &error),
                "re-entry after incomplete dwell evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "re-entry creates a new dwell target") ||
        !expect(output.in_region_targets[0].dwell_ms == 0, "re-entry starts dwell from zero") ||
        !expect(output.event_sequence == 0, "re-entry after incomplete dwell has no event"))
        return false;
    return true;
}

bool test_prediction_grace_and_gap_reset()
{
    IntrusionFrame output;
    std::string error;
    IntrusionEvaluator evaluator(make_config());

    if (!expect(evaluator.update(make_frame_with_tracks(
                    1, 1000, std::vector<TrackObject>(1, make_person(
                                                        7, 100, 100, 200, 300,
                                                        TrackerBBoxSource::Predicted))),
                &output, &error),
                "predicted-only entry evaluates: " + error) ||
        !expect(output.in_region_targets.empty(), "predicted-only track cannot start dwelling") ||
        !expect(output.event_sequence == 0, "predicted-only track cannot create an event"))
        return false;

    if (!expect(evaluator.update(make_frame(2, 2000, 100, 100, 200, 300), &output, &error),
                "observed entry after prediction evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "observed track starts dwelling") ||
        !expect(output.in_region_targets[0].dwell_ms == 0,
                "observed entry starts dwell at its own timestamp"))
        return false;

    if (!expect(evaluator.update(make_frame_with_tracks(
                    3, 2500, std::vector<TrackObject>(1, make_person(
                                                          7, 100, 100, 200, 300,
                                                          TrackerBBoxSource::Predicted))),
                &output, &error),
                "short prediction gap evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "short prediction gap preserves dwelling") ||
        !expect(output.in_region_targets[0].bbox_source == TrackerBBoxSource::Predicted,
                "short prediction gap exposes predicted source") ||
        !expect(output.in_region_targets[0].dwell_ms == 500,
                "short prediction gap keeps capture-time dwell") ||
        !expect(output.active_alarms.empty(), "short prediction gap does not trigger alarm"))
        return false;

    if (!expect(evaluator.update(make_frame_with_tracks(
                    4, 3501, std::vector<TrackObject>(1, make_person(
                                                          7, 100, 100, 200, 300,
                                                          TrackerBBoxSource::Predicted))),
                &output, &error),
                "long prediction gap evaluates: " + error) ||
        !expect(output.in_region_targets.empty(), "long prediction gap clears dwelling") ||
        !expect(output.active_alarms.empty(), "long prediction gap has no active alarm") ||
        !expect(output.event_sequence == 0, "long prediction gap does not create an event"))
        return false;

    if (!expect(evaluator.update(make_frame(5, 4500, 100, 100, 200, 300), &output, &error),
                "observed track after long gap evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "observed track after long gap re-enters") ||
        !expect(output.in_region_targets[0].dwell_ms == 0,
                "observed track after long gap starts from zero"))
        return false;

    IntrusionEvaluator missing_evaluator(make_config());
    if (!expect(missing_evaluator.update(make_frame(1, 1000, 100, 100, 200, 300),
                                         &output, &error),
                "missing frame entry evaluates: " + error) ||
        !expect(missing_evaluator.update(make_empty_frame(2, 1500), &output, &error),
                "short missing frame gap evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1,
                "short missing frame gap preserves dwelling") ||
        !expect(output.in_region_targets[0].dwell_ms == 500,
                "short missing frame gap keeps dwell progress") ||
        !expect(missing_evaluator.update(make_empty_frame(3, 2101), &output, &error),
                "long missing frame gap evaluates: " + error) ||
        !expect(output.in_region_targets.empty(),
                "long missing frame gap clears dwelling") ||
        !expect(output.event_sequence == 0,
                "long missing frame gap does not create an event") ||
        !expect(missing_evaluator.update(make_frame(4, 3100, 100, 100, 200, 300),
                                         &output, &error),
                "observed re-entry after missing frame gap evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1,
                "observed re-entry after missing frame gap starts target") ||
        !expect(output.in_region_targets[0].dwell_ms == 0,
                "observed re-entry after missing frame gap starts from zero"))
        return false;

    IntrusionEvaluator threshold_evaluator(make_config());
    if (!expect(threshold_evaluator.update(make_frame(1, 1000, 100, 100, 200, 300),
                                           &output, &error),
                "prediction threshold entry evaluates: " + error) ||
        !expect(threshold_evaluator.update(make_frame(2, 5000, 100, 100, 200, 300),
                                           &output, &error),
                "prediction threshold observed setup evaluates: " + error) ||
        !expect(threshold_evaluator.update(make_frame_with_tracks(
                    3, 6000, std::vector<TrackObject>(1, make_person(
                                                          7, 100, 100, 200, 300,
                                                          TrackerBBoxSource::Predicted))),
                &output, &error),
                "predicted threshold frame evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "predicted threshold frame preserves dwell") ||
        !expect(output.in_region_targets[0].dwell_ms == 5000,
                "predicted threshold frame reaches dwell duration") ||
        !expect(output.active_alarms.empty(), "predicted threshold frame cannot trigger alarm") ||
        !expect(output.event_sequence == 0, "predicted threshold frame has no event"))
        return false;

    if (!expect(threshold_evaluator.update(make_frame(4, 6001, 100, 100, 200, 300),
                                           &output, &error),
                "observed frame after predicted threshold evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1,
                "observed frame after predicted threshold triggers alarm") ||
        !expect(output.event_sequence == 1,
                "observed frame after predicted threshold creates one event"))
        return false;
    return true;
}

bool test_alarm_cycle_boundary_and_reentry()
{
    IntrusionEvaluator evaluator(make_config());
    IntrusionFrame output;
    std::string error;

    if (!expect(evaluator.update(make_frame(1, 1000, 100, 100, 200, 300), &output, &error),
                "alarm cycle entry evaluates: " + error) ||
        !expect(evaluator.update(make_frame(2, 6000, 100, 100, 200, 300), &output, &error),
                "alarm cycle threshold evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1, "alarm cycle has one active alarm") ||
        !expect(output.event_sequence == 1, "alarm cycle creates one alarm event"))
        return false;

    if (!expect(evaluator.update(make_frame(3, 7000, 274, 100, 374, 300), &output, &error),
                "boundary jitter evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1,
                "boundary jitter keeps the target in the region") ||
        !expect(output.active_alarms.size() == 1,
                "boundary jitter keeps the alarm active") ||
        !expect(output.recent_events.size() == 1,
                "boundary jitter does not create a duplicate event") ||
        !expect(output.event_sequence == 1,
                "boundary jitter does not advance event sequence"))
        return false;

    if (!expect(evaluator.update(make_frame(4, 8000, 276, 100, 376, 300), &output, &error),
                "true boundary exit evaluates: " + error) ||
        !expect(output.in_region_targets.empty(), "true boundary exit clears target") ||
        !expect(output.active_alarms.empty(), "true boundary exit clears active alarm") ||
        !expect(output.recent_events.size() == 2,
                "true boundary exit appends one clear event") ||
        !expect(output.recent_events[1].event_type == "cleared",
                "true boundary exit emits cleared event") ||
        !expect(output.event_sequence == 2,
                "true boundary exit advances event sequence once"))
        return false;

    if (!expect(evaluator.update(make_frame(5, 9000, 100, 100, 200, 300), &output, &error),
                "alarm cycle re-entry evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1, "re-entry exposes a new target") ||
        !expect(output.in_region_targets[0].dwell_ms == 0,
                "re-entry resets dwell progress") ||
        !expect(output.active_alarms.empty(), "re-entry does not retain old alarm") ||
        !expect(output.event_sequence == 2, "re-entry does not create an early event"))
        return false;

    if (!expect(evaluator.update(make_frame(6, 14000, 100, 100, 200, 300), &output, &error),
                "second alarm cycle threshold evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1,
                "second alarm cycle has one active alarm") ||
        !expect(output.recent_events.size() == 3,
                "second alarm cycle appends one new alarm event") ||
        !expect(output.recent_events[2].event_type == "alarmed",
                "second alarm cycle event is alarmed") ||
        !expect(output.recent_events[2].event_sequence == 3,
                "second alarm cycle receives a new sequence") ||
        !expect(output.event_sequence == 3,
                "second alarm cycle advances event sequence once"))
        return false;
    return true;
}

bool test_multiple_targets_are_independent()
{
    IntrusionEvaluator evaluator(make_config());
    IntrusionFrame output;
    std::string error;
    std::vector<TrackObject> both_inside;
    both_inside.push_back(make_person(7, 100, 100, 200, 300));
    both_inside.push_back(make_person(8, 200, 100, 300, 300));

    if (!expect(evaluator.update(make_frame_with_tracks(1, 1000, both_inside), &output, &error),
                "multiple target entry evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 2,
                "multiple target entry exposes two dwelling targets") ||
        !expect(output.active_alarms.empty(), "multiple target entry has no alarms"))
        return false;

    if (!expect(evaluator.update(make_frame_with_tracks(2, 6000, both_inside), &output, &error),
                "multiple target threshold evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 2,
                "multiple target threshold exposes both targets") ||
        !expect(output.active_alarms.size() == 2,
                "multiple target threshold activates both alarms") ||
        !expect(output.recent_events.size() == 2,
                "multiple target threshold emits one event per target") ||
        !expect(output.event_sequence == 2,
                "multiple target threshold advances sequence per target"))
        return false;

    std::vector<TrackObject> one_inside;
    one_inside.push_back(make_person(7, 100, 100, 200, 300));
    one_inside.push_back(make_person(8, 276, 100, 376, 300));
    if (!expect(evaluator.update(make_frame_with_tracks(3, 8000, one_inside), &output, &error),
                "one target exit evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1,
                "one target exit leaves the other target in region") ||
        !expect(output.in_region_targets[0].track_id == 7,
                "one target exit preserves track seven") ||
        !expect(output.active_alarms.size() == 1,
                "one target exit leaves one active alarm") ||
        !expect(output.active_alarms[0].track_id == 7,
                "one target exit leaves track seven alarm active") ||
        !expect(output.active_alarms[0].event_sequence == 1,
                "remaining alarm keeps its original event sequence") ||
        !expect(output.recent_events.size() == 3,
                "one target exit emits only one clear event") ||
        !expect(output.recent_events[2].target.track_id == 8,
                "clear event belongs to the exited target") ||
        !expect(output.event_sequence == 3,
                "one target exit advances sequence once"))
        return false;
    return true;
}

bool test_track_id_replacement_does_not_reidentify()
{
    IntrusionEvaluator evaluator(make_config());
    IntrusionFrame output;
    std::string error;

    if (!expect(evaluator.update(make_frame(1, 1000, 100, 100, 200, 300), &output, &error),
                "original track entry evaluates: " + error) ||
        !expect(evaluator.update(make_frame(2, 6000, 100, 100, 200, 300), &output, &error),
                "original track alarm evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1 && output.active_alarms[0].track_id == 7,
                "original track is alarmed"))
        return false;

    std::vector<TrackObject> replacement;
    replacement.push_back(make_person(8, 100, 100, 200, 300));
    if (!expect(evaluator.update(make_frame_with_tracks(3, 7101, replacement), &output, &error),
                "replacement track evaluates: " + error) ||
        !expect(output.in_region_targets.size() == 1,
                "replacement exposes one new target") ||
        !expect(output.in_region_targets[0].track_id == 8,
                "replacement uses the new track id") ||
        !expect(output.in_region_targets[0].dwell_ms == 0,
                "replacement starts dwell from zero") ||
        !expect(output.in_region_targets[0].event_sequence == 0,
                "replacement does not inherit old event sequence") ||
        !expect(output.active_alarms.empty(), "replacement does not inherit old alarm") ||
        !expect(output.recent_events.size() == 2,
                "replacement clears the old alarm exactly once") ||
        !expect(output.recent_events[1].event_type == "cleared" &&
                    output.recent_events[1].target.track_id == 7,
                "replacement clear event belongs to the old track") ||
        !expect(output.event_sequence == 2,
                "replacement advances sequence only for old clear"))
        return false;

    if (!expect(evaluator.update(make_frame_with_tracks(4, 12101, replacement), &output, &error),
                "replacement threshold evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1 && output.active_alarms[0].track_id == 8,
                "replacement can alarm after its own dwell") ||
        !expect(output.recent_events.size() == 3,
                "replacement appends one new alarm event") ||
        !expect(output.recent_events[2].target.track_id == 8 &&
                    output.recent_events[2].event_type == "alarmed",
                "replacement alarm event belongs to the new track") ||
        !expect(output.event_sequence == 3,
                "replacement alarm gets a fresh event sequence"))
        return false;
    return true;
}
}

int main()
{
    IntrusionRuleConfig config = make_config();

    std::string error;
    if (!expect(validate_intrusion_rule_config(config, &error), "default event rule validates: " + error))
        return 1;

    IntrusionEvaluator evaluator(config);
    IntrusionFrame output;

    if (!expect(evaluator.update(make_frame(1, 1000, 100, 100, 200, 300), &output, &error),
                "first in-region frame evaluates: " + error))
        return 1;
    if (!expect(output.in_region_targets.size() == 1, "first frame exposes one in-region target") ||
        !expect(output.in_region_targets[0].state == IntrusionTargetState::Dwelling,
                "first frame starts dwelling") ||
        !expect(output.in_region_targets[0].dwell_ms == 0, "dwell starts at capture timestamp") ||
        !expect(output.active_alarms.empty(), "first frame has no active alarm"))
        return 1;

    if (!expect(evaluator.update(make_frame(2, 5999, 100, 100, 200, 300), &output, &error),
                "pre-threshold frame evaluates: " + error) ||
        !expect(output.in_region_targets[0].state == IntrusionTargetState::Dwelling,
                "pre-threshold target remains dwelling") ||
        !expect(output.in_region_targets[0].dwell_ms == 4999, "pre-threshold dwell uses capture time") ||
        !expect(output.active_alarms.empty(), "pre-threshold frame has no active alarm"))
        return 1;

    if (!expect(evaluator.update(make_frame(3, 6000, 100, 100, 200, 300), &output, &error),
                "threshold frame evaluates: " + error) ||
        !expect(output.in_region_targets[0].state == IntrusionTargetState::Alarmed,
                "threshold frame becomes alarmed") ||
        !expect(output.in_region_targets[0].dwell_ms == 5000, "threshold dwell is five seconds") ||
        !expect(output.active_alarms.size() == 1, "threshold frame exposes one active alarm") ||
        !expect(output.recent_events.size() == 1, "threshold frame exposes one recent event") ||
        !expect(output.event_sequence == 1, "first alarm advances event sequence"))
        return 1;

    if (!expect(evaluator.update(make_frame(4, 7000, 100, 100, 200, 300), &output, &error),
                "held alarm frame evaluates: " + error) ||
        !expect(output.active_alarms.size() == 1, "held target keeps one active alarm") ||
        !expect(output.recent_events.size() == 1, "held target does not duplicate alarm event") ||
        !expect(output.event_sequence == 1, "held target does not advance event sequence"))
        return 1;

    if (!expect(test_incomplete_dwell_does_not_emit_event(),
                "incomplete dwell scenario passes"))
        return 1;

    if (!expect(test_config_validation_and_evaluator_failures(),
                "config validation and evaluator failure scenario passes"))
        return 1;

    if (!expect(test_prediction_grace_and_gap_reset(),
                "prediction grace and gap scenario passes"))
        return 1;

    if (!expect(test_alarm_cycle_boundary_and_reentry(),
                "alarm cycle boundary and re-entry scenario passes"))
        return 1;

    if (!expect(test_multiple_targets_are_independent(),
                "multiple target independence scenario passes"))
        return 1;

    if (!expect(test_track_id_replacement_does_not_reidentify(),
                "track id replacement scenario passes"))
        return 1;

    std::cout << "SCENARIO happy_path track_id=7 dwell_ms=5000 event_sequence=1" << std::endl;
    return 0;
}
