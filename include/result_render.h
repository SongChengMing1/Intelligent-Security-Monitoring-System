#ifndef RKNN_DETECT_RESULT_RENDER_H_
#define RKNN_DETECT_RESULT_RENDER_H_

#include "opencv2/core/core.hpp"

#include "perf_stats.h"
#include "postprocess.h"

void render_and_save_results(cv::Mat *orig_img, const detect_result_group_t *detect_result_group,
                             bool save_result, perf_stats_t *perf_stats);
void render_and_save_results_to_path(cv::Mat *orig_img, const detect_result_group_t *detect_result_group,
                                     bool save_result, const char *output_path, perf_stats_t *perf_stats);

#endif
