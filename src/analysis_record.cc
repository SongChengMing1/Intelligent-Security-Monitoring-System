#include "analysis_record.h"

const char *observation_origin_to_string(ObservationOrigin origin)
{
    switch (origin)
    {
    case ObservationOrigin::HighPass:
        return "high_pass";
    case ObservationOrigin::LowOnly:
        return "low_only";
    }
    return "unknown";
}

const char *observation_rejection_reason_to_string(ObservationRejectionReason reason)
{
    switch (reason)
    {
    case ObservationRejectionReason::ClassNotAllowed:
        return "class_not_allowed";
    case ObservationRejectionReason::BelowMinWidth:
        return "below_min_width";
    case ObservationRejectionReason::BelowMinHeight:
        return "below_min_height";
    case ObservationRejectionReason::BelowMinArea:
        return "below_min_area";
    case ObservationRejectionReason::OutsideRoi:
        return "outside_roi";
    case ObservationRejectionReason::InsideEdgeMargin:
        return "inside_edge_margin";
    case ObservationRejectionReason::CapacityLimit:
        return "capacity_limit";
    case ObservationRejectionReason::InvalidBBox:
        return "invalid_bbox";
    case ObservationRejectionReason::BelowTrackerLowThreshold:
        return "below_tracker_low_threshold";
    }
    return "unknown";
}

const char *recorded_image_state_to_string(RecordedImageState state)
{
    switch (state)
    {
    case RecordedImageState::None:
        return "none";
    case RecordedImageState::Exact:
        return "exact";
    case RecordedImageState::Unavailable:
        return "unavailable";
    case RecordedImageState::Dropped:
        return "dropped";
    }
    return "unknown";
}
