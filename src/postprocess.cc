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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <vector>
#include <set>
#include "postprocess.h"
#include <stdint.h>
#define LABEL_NAME_TXT_PATH "./model/coco_80_labels_list.txt"

static char *labels[OBJ_CLASS_NUM];
static bool g_log_postprocess_candidates = false;

inline static int clamp(float val, int min, int max)
{
    return val > min ? (val < max ? val : max) : min;
}

static int map_model_x_to_source(float model_x, const preprocess_transform_t *transform)
{
    if (transform == NULL || transform->scale_x <= 0.0f)
    {
        return (int)model_x;
    }
    float source_x = (model_x - transform->pad_x) / transform->scale_x;
    return clamp(source_x, 0, transform->src_width);
}

static int map_model_y_to_source(float model_y, const preprocess_transform_t *transform)
{
    if (transform == NULL || transform->scale_y <= 0.0f)
    {
        return (int)model_y;
    }
    float source_y = (model_y - transform->pad_y) / transform->scale_y;
    return clamp(source_y, 0, transform->src_height);
}

static char *readLine(FILE *fp, char *buffer, int *len)
{
    int ch;
    int i = 0;
    size_t buff_len = 0;

    buffer = (char *)malloc(buff_len + 1);
    if (!buffer)
        return NULL;

    while ((ch = fgetc(fp)) != '\n' && ch != EOF)
    {
        buff_len++;
        void *tmp = realloc(buffer, buff_len + 1);
        if (tmp == NULL)
        {
            free(buffer);
            return NULL;
        }
        buffer = (char *)tmp;
        buffer[i++] = (char)ch;
    }
    buffer[i] = '\0';
    *len = (int)buff_len;

    if (ch == EOF && (i == 0 || ferror(fp)))
    {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static int readLines(const char *fileName, char *lines[], int max_line)
{
    FILE *file = fopen(fileName, "r");
    if (file == NULL)
    {
        printf("Open %s fail!\n", fileName);
        return -1;
    }

    char *line = NULL;
    int line_len = 0;
    int count = 0;
    while (count < max_line && (line = readLine(file, line, &line_len)) != NULL)
    {
        lines[count++] = line;
        line = NULL;
    }
    fclose(file);
    return count;
}

static int loadLabelName(const char *locationFilename, char *label[])
{
    printf("loadLabelName %s\n", locationFilename);
    return readLines(locationFilename, label, OBJ_CLASS_NUM) < 0 ? -1 : 0;
}

static float calculate_overlap(float xmin0, float ymin0, float xmax0, float ymax0,
                               float xmin1, float ymin1, float xmax1, float ymax1)
{
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0f);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0f);
    float intersection = w * h;
    float union_area = (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
                       (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) - intersection;
    return union_area <= 0.f ? 0.f : intersection / union_area;
}

static int nms(int validCount, std::vector<float> &outputLocations,
               const std::vector<int> &classIds, std::vector<int> &order,
               int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
        {
            continue;
        }
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = xmin0 + outputLocations[n * 4 + 2];
            float ymax0 = ymin0 + outputLocations[n * 4 + 3];
            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = xmin1 + outputLocations[m * 4 + 2];
            float ymax1 = ymin1 + outputLocations[m * 4 + 3];
            if (calculate_overlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1) > threshold)
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right,
                                     std::vector<int> &indices)
{
    float key;
    int key_index;
    int low = left;
    int high = right;
    if (left < right)
    {
        key_index = indices[left];
        key = input[left];
        while (low < high)
        {
            while (low < high && input[high] <= key)
                --high;
            input[low] = input[high];
            indices[low] = indices[high];
            while (low < high && input[low] >= key)
                ++low;
            input[high] = input[low];
            indices[high] = indices[low];
        }
        input[low] = key;
        indices[low] = key_index;
        quick_sort_indice_inverse(input, left, low - 1, indices);
        quick_sort_indice_inverse(input, low + 1, right, indices);
    }
    return low;
}

static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
{
    return ((float)qnt - (float)zp) * scale;
}

static int ensure_labels_loaded()
{
    if (labels[0] != nullptr)
    {
        return 0;
    }
    return loadLabelName(LABEL_NAME_TXT_PATH, labels);
}

static float clamp01(float value)
{
    if (value < 0.0f)
    {
        return 0.0f;
    }
    if (value > 1.0f)
    {
        return 1.0f;
    }
    return value;
}

static int process_yolov6_stride(int8_t *box_input, int8_t *cls_input, int8_t *obj_input,
                                 int grid_h, int grid_w, int stride,
                                 std::vector<float> &boxes, std::vector<float> &objProbs,
                                 std::vector<int> &classId, float threshold,
                                 int32_t box_zp, float box_scale,
                                 int32_t cls_zp, float cls_scale,
                                 int32_t obj_zp, float obj_scale)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int grid_index = i * grid_w + j;
            // This exported YOLOv6n graph uses the third output as a class-confidence
            // sum/clip prefilter, not independent objectness.
            float prefilter_conf = clamp01(deqnt_affine_to_f32(obj_input[grid_index], obj_zp, obj_scale));
            if (prefilter_conf < threshold)
            {
                continue;
            }

            int maxClassId = 0;
            float maxClassProb = clamp01(deqnt_affine_to_f32(cls_input[grid_index], cls_zp, cls_scale));
            for (int k = 1; k < OBJ_CLASS_NUM; ++k)
            {
                float prob = clamp01(deqnt_affine_to_f32(cls_input[k * grid_len + grid_index], cls_zp, cls_scale));
                if (prob > maxClassProb)
                {
                    maxClassId = k;
                    maxClassProb = prob;
                }
            }
            if (maxClassProb < threshold)
            {
                continue;
            }

            float left = fmaxf(0.0f, deqnt_affine_to_f32(box_input[grid_index], box_zp, box_scale));
            float top = fmaxf(0.0f, deqnt_affine_to_f32(box_input[grid_len + grid_index], box_zp, box_scale));
            float right = fmaxf(0.0f, deqnt_affine_to_f32(box_input[2 * grid_len + grid_index], box_zp, box_scale));
            float bottom = fmaxf(0.0f, deqnt_affine_to_f32(box_input[3 * grid_len + grid_index], box_zp, box_scale));

            float center_x = ((float)j + 0.5f) * (float)stride;
            float center_y = ((float)i + 0.5f) * (float)stride;
            float x1 = center_x - left * (float)stride;
            float y1 = center_y - top * (float)stride;
            float w = (left + right) * (float)stride;
            float h = (top + bottom) * (float)stride;

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(w);
            boxes.push_back(h);
            objProbs.push_back(maxClassProb);
            classId.push_back(maxClassId);
            validCount++;
        }
    }

    return validCount;
}

int yolov6_post_process(int8_t **inputs, int model_in_h, int model_in_w,
                        float conf_threshold, float nms_threshold, const preprocess_transform_t *transform,
                        std::vector<int32_t> &qnt_zps, std::vector<float> &qnt_scales,
                        detect_result_group_t *group)
{
    if (ensure_labels_loaded() < 0)
    {
        return -1;
    }
    memset(group, 0, sizeof(detect_result_group_t));

    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;

    int validCount0 = process_yolov6_stride(inputs[0], inputs[1], inputs[2], model_in_h / 8, model_in_w / 8, 8,
                                            filterBoxes, objProbs, classId, conf_threshold,
                                            qnt_zps[0], qnt_scales[0], qnt_zps[1], qnt_scales[1],
                                            qnt_zps[2], qnt_scales[2]);
    int validCount1 = process_yolov6_stride(inputs[3], inputs[4], inputs[5], model_in_h / 16, model_in_w / 16, 16,
                                            filterBoxes, objProbs, classId, conf_threshold,
                                            qnt_zps[3], qnt_scales[3], qnt_zps[4], qnt_scales[4],
                                            qnt_zps[5], qnt_scales[5]);
    int validCount2 = process_yolov6_stride(inputs[6], inputs[7], inputs[8], model_in_h / 32, model_in_w / 32, 32,
                                            filterBoxes, objProbs, classId, conf_threshold,
                                            qnt_zps[6], qnt_scales[6], qnt_zps[7], qnt_scales[7],
                                            qnt_zps[8], qnt_scales[8]);

    int validCount = validCount0 + validCount1 + validCount2;
    if (g_log_postprocess_candidates)
    {
        printf("yolov6 candidates before nms: stride8=%d, stride16=%d, stride32=%d, total=%d\n",
               validCount0, validCount1, validCount2, validCount);
    }

    if (validCount <= 0)
    {
        if (g_log_postprocess_candidates)
        {
            printf("yolov6 results after nms: 0\n");
        }
        return 0;
    }

    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }

    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));
    for (auto c : class_set)
    {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    int last_count = 0;
    group->count = 0;
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0];
        float y1 = filterBoxes[n * 4 + 1];
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[n];

        group->results[last_count].box.left = map_model_x_to_source(clamp(x1, 0, model_in_w), transform);
        group->results[last_count].box.top = map_model_y_to_source(clamp(y1, 0, model_in_h), transform);
        group->results[last_count].box.right = map_model_x_to_source(clamp(x2, 0, model_in_w), transform);
        group->results[last_count].box.bottom = map_model_y_to_source(clamp(y2, 0, model_in_h), transform);
        group->results[last_count].class_id = id;
        group->results[last_count].prop = obj_conf;
        char *label = labels[id];
        strncpy(group->results[last_count].name, label, OBJ_NAME_MAX_SIZE);
        group->results[last_count].name[OBJ_NAME_MAX_SIZE - 1] = '\0';
        last_count++;
    }

    group->count = last_count;
    if (g_log_postprocess_candidates)
    {
        printf("yolov6 results after nms: %d (max=%d)\n", group->count, OBJ_NUMB_MAX_SIZE);
    }

    return 0;
}

void set_postprocess_candidate_logging(bool enabled)
{
    g_log_postprocess_candidates = enabled;
}

void deinitPostProcess() {

    for (int i=0; i<OBJ_CLASS_NUM; i++) {
        if(labels[i] != nullptr) {
            free(labels[i]);
            labels[i] = nullptr;
        }
    }
}
