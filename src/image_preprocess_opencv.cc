#include "image_preprocess_opencv.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

static bool g_opencv_preprocess_logging = true;

void set_opencv_preprocess_logging(bool enabled)
{
    g_opencv_preprocess_logging = enabled;
}

static double opencv_preprocess_elapsed_ms(struct timeval start, struct timeval stop)
{
    double start_us = start.tv_sec * 1000000.0 + start.tv_usec;
    double stop_us = stop.tv_sec * 1000000.0 + stop.tv_usec;
    return (stop_us - start_us) / 1000.0;
}

int preprocess_frame_opencv(const cv::Mat &bgr_frame, int model_width, int model_height,
                            int model_channel, void *input_buf, preprocess_mode_t mode,
                            preprocess_transform_t *transform,
                            bool dump_preprocess, const char *dump_path, double *preprocess_ms)
{
    if (bgr_frame.empty())
    {
        printf("preprocess_frame_opencv failed: empty frame\n");
        return -1;
    }
    if (bgr_frame.channels() != 3)
    {
        printf("preprocess_frame_opencv failed: expected 3-channel BGR frame, got %d channels\n",
               bgr_frame.channels());
        return -1;
    }
    if (model_width <= 0 || model_height <= 0 || model_channel != 3 || input_buf == NULL)
    {
        printf("preprocess_frame_opencv failed: invalid model input shape %dx%dx%d or input buffer\n",
               model_width, model_height, model_channel);
        return -1;
    }

    struct timeval start_time;
    struct timeval stop_time;
    gettimeofday(&start_time, NULL);

    cv::Mat rgb_frame;
    cv::cvtColor(bgr_frame, rgb_frame, cv::COLOR_BGR2RGB);

    cv::Mat resized_rgb(cv::Size(model_width, model_height), CV_8UC3, input_buf);
    if (mode == PREPROCESS_MODE_LETTERBOX)
    {
        float scale = std::min((float)model_width / (float)bgr_frame.cols,
                               (float)model_height / (float)bgr_frame.rows);
        int resized_w = std::max(1, (int)roundf((float)bgr_frame.cols * scale));
        int resized_h = std::max(1, (int)roundf((float)bgr_frame.rows * scale));
        int pad_x = (model_width - resized_w) / 2;
        int pad_y = (model_height - resized_h) / 2;

        resized_rgb.setTo(cv::Scalar(114, 114, 114));
        cv::Mat resized_area = resized_rgb(cv::Rect(pad_x, pad_y, resized_w, resized_h));
        cv::resize(rgb_frame, resized_area, cv::Size(resized_w, resized_h), 0, 0, cv::INTER_LINEAR);
        if (transform)
        {
            memset(transform, 0, sizeof(*transform));
            transform->mode = mode;
            transform->src_width = bgr_frame.cols;
            transform->src_height = bgr_frame.rows;
            transform->model_width = model_width;
            transform->model_height = model_height;
            transform->scale_x = scale;
            transform->scale_y = scale;
            transform->pad_x = (float)pad_x;
            transform->pad_y = (float)pad_y;
        }
        if (g_opencv_preprocess_logging)
        {
            printf("camera preprocess path: opencv_bgr2rgb_letterbox src=%dx%d resized=%dx%d pad=%d,%d\n",
                   bgr_frame.cols, bgr_frame.rows, resized_w, resized_h, pad_x, pad_y);
        }
    }
    else if (bgr_frame.cols == model_width && bgr_frame.rows == model_height)
    {
        memcpy(input_buf, rgb_frame.data, model_width * model_height * model_channel);
        init_resize_preprocess_transform(transform, bgr_frame.cols, bgr_frame.rows,
                                         model_width, model_height);
        if (g_opencv_preprocess_logging)
        {
            printf("camera preprocess path: opencv_bgr2rgb_direct_copy\n");
        }
    }
    else
    {
        cv::resize(rgb_frame, resized_rgb, cv::Size(model_width, model_height), 0, 0, cv::INTER_LINEAR);
        init_resize_preprocess_transform(transform, bgr_frame.cols, bgr_frame.rows,
                                         model_width, model_height);
        if (g_opencv_preprocess_logging)
        {
            printf("camera preprocess path: opencv_bgr2rgb_resize\n");
        }
    }

    gettimeofday(&stop_time, NULL);
    double elapsed = opencv_preprocess_elapsed_ms(start_time, stop_time);
    if (preprocess_ms)
    {
        *preprocess_ms = elapsed;
    }
    if (dump_preprocess)
    {
        const char *path = dump_path && dump_path[0] != '\0' ? dump_path : "camera_preprocess.jpg";
        cv::Mat dump_bgr;
        cv::cvtColor(resized_rgb, dump_bgr, cv::COLOR_RGB2BGR);
        if (!cv::imwrite(path, dump_bgr))
        {
            printf("preprocess_frame_opencv warning: failed to save %s\n", path);
        }
        else
        {
            printf("saved preprocessed frame to %s\n", path);
        }
    }

    if (g_opencv_preprocess_logging)
    {
        printf("camera preprocess: mode=%s src=%dx%d dst=%dx%d channel=%d scale_x=%.6f scale_y=%.6f pad_x=%.1f pad_y=%.1f preprocess_ms=%.3f\n",
               preprocess_mode_to_string(transform ? transform->mode : mode),
               bgr_frame.cols, bgr_frame.rows, model_width, model_height, model_channel,
               transform ? transform->scale_x : 0.0f, transform ? transform->scale_y : 0.0f,
               transform ? transform->pad_x : 0.0f, transform ? transform->pad_y : 0.0f, elapsed);
    }
    return 0;
}
