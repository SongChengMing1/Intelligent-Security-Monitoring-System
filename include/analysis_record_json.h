#ifndef RKNN_DETECT_ANALYSIS_RECORD_JSON_H_
#define RKNN_DETECT_ANALYSIS_RECORD_JSON_H_

#include <string>

#include "analysis_record.h"

std::string analysis_record_to_json(const AnalysisRecord &record);
bool analysis_record_from_json(const std::string &json_text, AnalysisRecord *record, std::string *error_message);

#endif
