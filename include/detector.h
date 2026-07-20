#ifndef RKNN_DETECT_DETECTOR_H_
#define RKNN_DETECT_DETECTOR_H_

#include <vector>

#include "perf_stats.h"
#include "postprocess.h"
#include "rknn_api.h"

unsigned char *load_model(const char *filename, int *model_size);
int query_sdk_version(rknn_context ctx);
int query_io_num(rknn_context ctx, rknn_input_output_num *io_num);
int query_input_attrs(rknn_context ctx, rknn_tensor_attr *input_attrs, int input_num);
int query_output_attrs(rknn_context ctx, rknn_tensor_attr *output_attrs, int output_num);
void get_model_input_shape(const rknn_tensor_attr *input_attr, int *height, int *width, int *channel);
int run_rknn_inference(rknn_context ctx, rknn_input_output_num io_num, rknn_input *inputs,
                       rknn_output *outputs, perf_stats_t *perf_stats);
int run_yolov6_postprocess(rknn_output *outputs, const rknn_tensor_attr *output_attrs, int output_num,
                           int height, int width, float box_conf_threshold,
                           float nms_threshold, const preprocess_transform_t *transform,
                           std::vector<int32_t> &out_zps, std::vector<float> &out_scales,
                           detect_result_group_t *detect_result_group, perf_stats_t *perf_stats);
void set_detector_inference_logging(bool enabled);
void release_runtime(rknn_context ctx, unsigned char *model_data, void *resize_buf);

#endif  // RKNN_DETECT_DETECTOR_H_
