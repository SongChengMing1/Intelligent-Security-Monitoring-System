#ifndef RKNN_DETECT_ANALYSIS_RECORD_H_
#define RKNN_DETECT_ANALYSIS_RECORD_H_

#include <stdint.h>

#include <string>
#include <memory>
#include <vector>

#include "object_tracker.h"

static const int kAnalysisRecordSchemaVersion = 1;
static const int kRecordingManifestSchemaVersion = 1;
static const int kRecordingSessionStatusSchemaVersion = 1;
static const int kReplayManifestSchemaVersion = 1;
static const int kReplayStatusSchemaVersion = 1;

enum class ObservationOrigin
{
    HighPass,
    LowOnly,
};

enum class ObservationRejectionReason
{
    ClassNotAllowed,
    BelowMinWidth,
    BelowMinHeight,
    BelowMinArea,
    OutsideRoi,
    InsideEdgeMargin,
    CapacityLimit,
    InvalidBBox,
    BelowTrackerLowThreshold,
};

enum class RecordedImageState
{
    None,
    Exact,
    Unavailable,
    Dropped,
};

const char *observation_origin_to_string(ObservationOrigin origin);
const char *observation_rejection_reason_to_string(ObservationRejectionReason reason);
const char *recorded_image_state_to_string(RecordedImageState state);

struct AnalysisObservation
{
    uint32_t observation_id;
    ObservationOrigin origin;
    int class_id;
    std::string class_name;
    float score;
    TrackerBBox bbox;
    bool admitted;
    std::vector<ObservationRejectionReason> rejection_reasons;

    AnalysisObservation()
        : observation_id(0),
          origin(ObservationOrigin::HighPass),
          class_id(-1),
          class_name(),
          score(0.0f),
          bbox(),
          admitted(false),
          rejection_reasons()
    {
    }
};

struct RecordedImageReference
{
    RecordedImageState state;
    std::string relative_path;
    long long capture_frame_id;

    RecordedImageReference()
        : state(RecordedImageState::None), relative_path(), capture_frame_id(0)
    {
    }
};

struct RecordedFramePayload
{
    int width;
    int height;
    int channels;
    std::vector<unsigned char> bgr;

    RecordedFramePayload() : width(0), height(0), channels(3), bgr() {}
};

struct AnalysisTimings
{
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    double policy_ms;
    double tracker_ms;
    double recorder_enqueue_ms;

    AnalysisTimings()
        : preprocess_ms(0.0),
          inference_ms(0.0),
          postprocess_ms(0.0),
          policy_ms(0.0),
          tracker_ms(0.0),
          recorder_enqueue_ms(0.0)
    {
    }
};

struct AnalysisRecord
{
    int record_schema_version;
    std::string recording_session_id;
    std::string runtime_session_id;
    long long capture_frame_id;
    long long capture_timestamp_ms;
    long long record_timestamp_ms;
    int source_width;
    int source_height;
    std::vector<AnalysisObservation> observations;
    std::vector<uint32_t> tracker_input_observation_ids;
    TrackFrame recorded_tracks;
    std::string effective_profile_id;
    std::string effective_profile_hash;
    AnalysisTimings timings;
    RecordedImageReference image;
    std::shared_ptr<RecordedFramePayload> image_payload;

    AnalysisRecord()
        : record_schema_version(kAnalysisRecordSchemaVersion),
          recording_session_id(),
          runtime_session_id(),
          capture_frame_id(0),
          capture_timestamp_ms(0),
          record_timestamp_ms(0),
          source_width(0),
          source_height(0),
          observations(),
          tracker_input_observation_ids(),
          recorded_tracks(),
          effective_profile_id(),
          effective_profile_hash(),
          timings(),
          image(),
          image_payload()
    {
    }
};

#endif
