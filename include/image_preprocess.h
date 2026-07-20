#ifndef RKNN_DETECT_IMAGE_PREPROCESS_H_
#define RKNN_DETECT_IMAGE_PREPROCESS_H_

#include "opencv2/core/core.hpp"

#include "RgaUtils.h"
#include "im2d.h"
#include "perf_stats.h"
#include "rga.h"

int load_image_frame(const char *image_name, cv::Mat *orig_img, cv::Mat *rgb_img, int *img_width,
                     int *img_height, perf_stats_t *perf_stats);
void init_rga_context(rga_buffer_t *src, rga_buffer_t *dst, im_rect *src_rect, im_rect *dst_rect);
int prepare_input_buffer(const cv::Mat &rgb_img, int img_width, int img_height, int width, int height,
                         int channel, void *resize_buf, rga_buffer_t *src, rga_buffer_t *dst,
                         im_rect *src_rect, im_rect *dst_rect, bool dump_preprocess, perf_stats_t *perf_stats);

#endif
