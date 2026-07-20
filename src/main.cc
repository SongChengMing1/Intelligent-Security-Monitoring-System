// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <vector>

#define _BASETSD_H

#include "detector.h"
#include "image_preprocess.h"
#include "perf_stats.h"
#include "postprocess.h"
#include "result_render.h"

#define PERF_WITH_POST 1

static void print_usage(const char *program)
{
    printf("Usage: %s [--dump-preprocess] [--no-save-result] [--log-postprocess] "
           "[--box-thresh value] [--nms-thresh value] <rknn model> <jpg>\n",
           program);
}

static bool parse_threshold_arg(const char *name, const char *value, float *threshold)
{
    char *end = NULL;
    float parsed = strtof(value, &end);
    if (end == value || *end != '\0' || parsed <= 0.0f || parsed >= 1.0f)
    {
        printf("%s must be a float in (0, 1), got '%s'\n", name, value);
        return false;
    }
    *threshold = parsed;
    return true;
}

/*-------------------------------------------
                  Main Functions
-------------------------------------------*/
int main(int argc, char **argv)
{
    int status = 0;
    char *model_name = NULL;
    rknn_context ctx;
    size_t actual_size = 0;
    int img_width = 0;
    int img_height = 0;
    int img_channel = 0;
    float nms_threshold = NMS_THRESH;
    float box_conf_threshold = BOX_THRESH;
    struct timeval start_time, stop_time;
    int ret;
    bool dump_preprocess = false;
    bool save_result = true;
    bool log_postprocess = false;
    perf_stats_t perf_stats;
    memset(&perf_stats, 0, sizeof(perf_stats));

    // init rga context
    rga_buffer_t src;
    rga_buffer_t dst;
    im_rect src_rect;
    im_rect dst_rect;
    init_rga_context(&src, &dst, &src_rect, &dst_rect);

    std::vector<char *> positional_args;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--dump-preprocess") == 0)
        {
            dump_preprocess = true;
        }
        else if (strcmp(argv[i], "--no-save-result") == 0)
        {
            save_result = false;
        }
        else if (strcmp(argv[i], "--log-postprocess") == 0)
        {
            log_postprocess = true;
        }
        else if (strcmp(argv[i], "--box-thresh") == 0)
        {
            if (i + 1 >= argc || !parse_threshold_arg("--box-thresh", argv[++i], &box_conf_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else if (strcmp(argv[i], "--nms-thresh") == 0)
        {
            if (i + 1 >= argc || !parse_threshold_arg("--nms-thresh", argv[++i], &nms_threshold))
            {
                print_usage(argv[0]);
                return -1;
            }
        }
        else
        {
            positional_args.push_back(argv[i]);
        }
    }

    if (positional_args.size() != 2)
    {
        print_usage(argv[0]);
        return -1;
    }

    printf("post process config: model_type=yolov6, box_conf_threshold=%.2f, nms_threshold=%.2f, candidate_log=%s\n",
           box_conf_threshold, nms_threshold,
           log_postprocess ? "enabled" : "disabled");

    model_name = positional_args[0];
    char *image_name = positional_args[1];
    printf("runtime output config: dump_preprocess=%s, save_result=%s\n",
           dump_preprocess ? "enabled" : "disabled",
           save_result ? "enabled" : "disabled");

    cv::Mat orig_img;
    cv::Mat img;
    ret = load_image_frame(image_name, &orig_img, &img, &img_width, &img_height, &perf_stats);
    if (ret < 0)
    {
        return -1;
    }

    /* Create the neural network */
    printf("Loading mode...\n");
    int model_data_size = 0;
    unsigned char *model_data = load_model(model_name, &model_data_size);
    ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }

    ret = query_sdk_version(ctx);
    if (ret < 0)
    {
        return -1;
    }

    rknn_input_output_num io_num;
    ret = query_io_num(ctx, &io_num);
    if (ret < 0)
    {
        return -1;
    }

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    ret = query_input_attrs(ctx, input_attrs, io_num.n_input);
    if (ret < 0)
    {
        return -1;
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    ret = query_output_attrs(ctx, output_attrs, io_num.n_output);
    if (ret < 0)
    {
        return -1;
    }

    int channel = 3;
    int width = 0;
    int height = 0;
    get_model_input_shape(&input_attrs[0], &height, &width, &channel);

    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = width * height * channel;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0;

    void *resize_buf = malloc(height * width * channel);
    if (resize_buf == NULL)
    {
        printf("malloc resize_buf failed!\n");
        return -1;
    }

    ret = prepare_input_buffer(img, img_width, img_height, width, height, channel, resize_buf, &src, &dst, &src_rect,
                               &dst_rect, dump_preprocess, &perf_stats);
    if (ret < 0)
    {
        return -1;
    }

    inputs[0].buf = resize_buf;

    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 0;
    }

    ret = run_rknn_inference(ctx, io_num, inputs, outputs, &perf_stats);
    if (ret < 0)
    {
        return -1;
    }

    //post process
    preprocess_transform_t preprocess_transform;
    init_resize_preprocess_transform(&preprocess_transform, img_width, img_height, width, height);

    detect_result_group_t detect_result_group;
    set_postprocess_candidate_logging(log_postprocess);
    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    ret = run_yolov6_postprocess(outputs, output_attrs, io_num.n_output, height, width,
                                 box_conf_threshold, nms_threshold, &preprocess_transform, out_zps,
                                 out_scales, &detect_result_group, &perf_stats);
    if (ret < 0)
    {
        return -1;
    }

    render_and_save_results(&orig_img, &detect_result_group, save_result, &perf_stats);
    update_perf_totals(&perf_stats);
    print_perf_stats(&perf_stats);
    set_postprocess_candidate_logging(false);
    ret = rknn_outputs_release(ctx, io_num.n_output, outputs);

    // loop test
    int test_count = 10;
    gettimeofday(&start_time, NULL);
    for (int i = 0; i < test_count; ++i)
    {
        rknn_inputs_set(ctx, io_num.n_input, inputs);
        ret = rknn_run(ctx, NULL);
        ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
#if PERF_WITH_POST
        perf_stats_t loop_perf_stats;
        memset(&loop_perf_stats, 0, sizeof(loop_perf_stats));
        ret = run_yolov6_postprocess(outputs, output_attrs, io_num.n_output, height, width,
                                     box_conf_threshold, nms_threshold, &preprocess_transform, out_zps,
                                     out_scales, &detect_result_group, &loop_perf_stats);
        if (ret < 0)
        {
            rknn_outputs_release(ctx, io_num.n_output, outputs);
            return -1;
        }
#endif
        ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
    }
    gettimeofday(&stop_time, NULL);
    printf("loop count = %d , average run  %f ms\n", test_count,
           elapsed_ms(start_time, stop_time) / test_count);

    release_runtime(ctx, model_data, resize_buf);

    return 0;
}
