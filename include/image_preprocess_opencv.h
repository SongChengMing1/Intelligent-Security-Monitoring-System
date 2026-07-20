#ifndef RKNN_DETECT_IMAGE_PREPROCESS_OPENCV_H_
#define RKNN_DETECT_IMAGE_PREPROCESS_OPENCV_H_

#include "opencv2/core/core.hpp"

#include "preprocess_transform.h"

int preprocess_frame_opencv(const cv::Mat &bgr_frame, int model_width, int model_height,
                            int model_channel, void *input_buf, preprocess_mode_t mode,
                            preprocess_transform_t *transform,
                            bool dump_preprocess, const char *dump_path, double *preprocess_ms);
void set_opencv_preprocess_logging(bool enabled);

#endif
