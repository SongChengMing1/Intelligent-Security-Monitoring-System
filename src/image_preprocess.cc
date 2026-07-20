#include "image_preprocess.h"

#include <stdio.h>
#include <string.h>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

int load_image_frame(const char *image_name, cv::Mat *orig_img, cv::Mat *rgb_img, int *img_width,
                     int *img_height, perf_stats_t *perf_stats)
{
    struct timeval stage_start_time, stage_stop_time;

    printf("Read %s ...\n", image_name);
    *orig_img = cv::imread(image_name, 1);
    if (!orig_img->data)
    {
        printf("cv::imread %s fail!\n", image_name);
        return -1;
    }

    gettimeofday(&stage_start_time, NULL);
    cv::cvtColor(*orig_img, *rgb_img, cv::COLOR_BGR2RGB);
    gettimeofday(&stage_stop_time, NULL);
    perf_stats->preprocess_ms += elapsed_ms(stage_start_time, stage_stop_time);

    *img_width = rgb_img->cols;
    *img_height = rgb_img->rows;
    printf("img width = %d, img height = %d\n", *img_width, *img_height);
    return 0;
}

void init_rga_context(rga_buffer_t *src, rga_buffer_t *dst, im_rect *src_rect, im_rect *dst_rect)
{
    memset(src_rect, 0, sizeof(*src_rect));
    memset(dst_rect, 0, sizeof(*dst_rect));
    memset(src, 0, sizeof(*src));
    memset(dst, 0, sizeof(*dst));
}

int prepare_input_buffer(const cv::Mat &rgb_img, int img_width, int img_height, int width, int height,
                         int channel, void *resize_buf, rga_buffer_t *src, rga_buffer_t *dst,
                         im_rect *src_rect, im_rect *dst_rect, bool dump_preprocess, perf_stats_t *perf_stats)
{
    struct timeval stage_start_time, stage_stop_time;

    if (img_width == width && img_height == height)
    {
        gettimeofday(&stage_start_time, NULL);
        memcpy(resize_buf, rgb_img.data, width * height * channel);
        gettimeofday(&stage_stop_time, NULL);
        perf_stats->preprocess_ms += elapsed_ms(stage_start_time, stage_stop_time);
        printf("preprocess path: direct_copy\n");
    }
    else
    {
        gettimeofday(&stage_start_time, NULL);
        *src = wrapbuffer_virtualaddr((void *)rgb_img.data, img_width, img_height, RK_FORMAT_RGB_888);
        *dst = wrapbuffer_virtualaddr((void *)resize_buf, width, height, RK_FORMAT_RGB_888);
        int ret = imcheck(*src, *dst, *src_rect, *dst_rect);
        if (IM_STATUS_NOERROR != ret)
        {
            printf("%d, rga check error! %s\n", __LINE__, imStrError((IM_STATUS)ret));
        }
        IM_STATUS status = IM_STATUS_FAILED;
        if (IM_STATUS_NOERROR == ret)
        {
            status = imresize(*src, *dst);
        }
        if (IM_STATUS_NOERROR != status)
        {
            printf("rga resize failed: %s, fallback to opencv resize\n", imStrError(status));
            cv::Mat resize_img(cv::Size(width, height), CV_8UC3, resize_buf);
            cv::resize(rgb_img, resize_img, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
            printf("preprocess path: opencv_resize_fallback\n");
        }
        else
        {
            printf("preprocess path: rga_resize\n");
        }
        gettimeofday(&stage_stop_time, NULL);
        perf_stats->preprocess_ms += elapsed_ms(stage_start_time, stage_stop_time);
    }

    if (dump_preprocess)
    {
        cv::Mat resize_img(cv::Size(width, height), CV_8UC3, resize_buf);
        cv::imwrite("resize_input.jpg", resize_img);
    }
    return 0;
}
