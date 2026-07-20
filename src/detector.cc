#include "detector.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postprocess.h"

static bool g_detector_inference_logging = true;

void set_detector_inference_logging(bool enabled)
{
    g_detector_inference_logging = enabled;
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    int ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    fread(data, 1, sz, fp);
    return data;
}

unsigned char *load_model(const char *filename, int *model_size)
{
    FILE *fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    unsigned char *data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

int query_sdk_version(rknn_context ctx)
{
    rknn_sdk_version version;
    int ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return ret;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version, version.drv_version);
    return 0;
}

int query_io_num(rknn_context ctx, rknn_input_output_num *io_num)
{
    int ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, io_num, sizeof(rknn_input_output_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return ret;
    }
    printf("model input num: %d, output num: %d\n", io_num->n_input, io_num->n_output);
    return 0;
}

int query_input_attrs(rknn_context ctx, rknn_tensor_attr *input_attrs, int input_num)
{
    for (int i = 0; i < input_num; i++)
    {
        input_attrs[i].index = i;
        int ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            return ret;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }
    return 0;
}

int query_output_attrs(rknn_context ctx, rknn_tensor_attr *output_attrs, int output_num)
{
    for (int i = 0; i < output_num; i++)
    {
        output_attrs[i].index = i;
        int ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_query output attr error ret=%d\n", ret);
            return ret;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }
    return 0;
}

void get_model_input_shape(const rknn_tensor_attr *input_attr, int *height, int *width, int *channel)
{
    if (input_attr->fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        *channel = input_attr->dims[1];
        *width = input_attr->dims[2];
        *height = input_attr->dims[3];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        *width = input_attr->dims[1];
        *height = input_attr->dims[2];
        *channel = input_attr->dims[3];
    }

    printf("model input height=%d, width=%d, channel=%d\n", *height, *width, *channel);
}

int run_rknn_inference(rknn_context ctx, rknn_input_output_num io_num, rknn_input *inputs,
                       rknn_output *outputs, perf_stats_t *perf_stats)
{
    struct timeval start_time, stop_time;

    gettimeofday(&start_time, NULL);
    int ret = rknn_inputs_set(ctx, io_num.n_input, inputs);
    if (ret < 0)
    {
        printf("rknn_inputs_set error ret=%d\n", ret);
        return ret;
    }
    ret = rknn_run(ctx, NULL);
    if (ret < 0)
    {
        printf("rknn_run error ret=%d\n", ret);
        return ret;
    }
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
    gettimeofday(&stop_time, NULL);
    perf_stats->inference_ms = elapsed_ms(start_time, stop_time);
    if (ret < 0)
    {
        printf("rknn_outputs_get error ret=%d\n", ret);
        return ret;
    }
    if (g_detector_inference_logging)
    {
        printf("once run use %f ms\n", perf_stats->inference_ms);
    }
    return 0;
}

static bool tensor_attr_is_nchw(const rknn_tensor_attr *attr, int channels, int height, int width)
{
    return attr->n_dims == 4 && attr->fmt == RKNN_TENSOR_NCHW &&
           attr->dims[0] == 1 && attr->dims[1] == channels &&
           attr->dims[2] == height && attr->dims[3] == width;
}

static int check_yolov6_output_layout(const rknn_tensor_attr *output_attrs, int output_num, int height, int width)
{
    if (output_num != 9)
    {
        printf("unsupported yolov6 output layout: expected 9 outputs, got %d\n", output_num);
        return -1;
    }

    const int expected[9][3] = {
        {4, height / 8, width / 8},
        {OBJ_CLASS_NUM, height / 8, width / 8},
        {1, height / 8, width / 8},
        {4, height / 16, width / 16},
        {OBJ_CLASS_NUM, height / 16, width / 16},
        {1, height / 16, width / 16},
        {4, height / 32, width / 32},
        {OBJ_CLASS_NUM, height / 32, width / 32},
        {1, height / 32, width / 32},
    };

    for (int i = 0; i < output_num; ++i)
    {
        if (!tensor_attr_is_nchw(&output_attrs[i], expected[i][0], expected[i][1], expected[i][2]))
        {
            printf("unsupported yolov6 output[%d] layout: expected NCHW [1,%d,%d,%d], got fmt=%s dims=[%d,%d,%d,%d]\n",
                   i, expected[i][0], expected[i][1], expected[i][2],
                   get_format_string(output_attrs[i].fmt),
                   output_attrs[i].dims[0], output_attrs[i].dims[1],
                   output_attrs[i].dims[2], output_attrs[i].dims[3]);
            return -1;
        }
    }

    return 0;
}

int run_yolov6_postprocess(rknn_output *outputs, const rknn_tensor_attr *output_attrs, int output_num,
                           int height, int width, float box_conf_threshold,
                           float nms_threshold, const preprocess_transform_t *transform,
                           std::vector<int32_t> &out_zps, std::vector<float> &out_scales,
                           detect_result_group_t *detect_result_group, perf_stats_t *perf_stats)
{
    int ret = check_yolov6_output_layout(output_attrs, output_num, height, width);
    if (ret < 0)
    {
        return ret;
    }

    int8_t *output_bufs[9];
    for (int i = 0; i < 9; ++i)
    {
        output_bufs[i] = (int8_t *)outputs[i].buf;
    }

    struct timeval stage_start_time, stage_stop_time;
    gettimeofday(&stage_start_time, NULL);
    ret = yolov6_post_process(output_bufs, height, width, box_conf_threshold, nms_threshold, transform,
                              out_zps, out_scales, detect_result_group);
    gettimeofday(&stage_stop_time, NULL);
    perf_stats->postprocess_ms = elapsed_ms(stage_start_time, stage_stop_time);
    return ret;
}

void release_runtime(rknn_context ctx, unsigned char *model_data, void *resize_buf)
{
    deinitPostProcess();
    rknn_destroy(ctx);

    if (model_data)
    {
        free(model_data);
    }

    if (resize_buf)
    {
        free(resize_buf);
    }
}
