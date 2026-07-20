#include "preprocess_transform.h"

#include <stdio.h>
#include <string.h>

const char *preprocess_mode_to_string(preprocess_mode_t mode)
{
    switch (mode)
    {
    case PREPROCESS_MODE_RESIZE:
        return "resize";
    case PREPROCESS_MODE_LETTERBOX:
        return "letterbox";
    default:
        return "unknown";
    }
}

bool parse_preprocess_mode(const char *value, preprocess_mode_t *mode)
{
    if (value == NULL || mode == NULL)
    {
        return false;
    }
    if (strcmp(value, "resize") == 0)
    {
        *mode = PREPROCESS_MODE_RESIZE;
        return true;
    }
    if (strcmp(value, "letterbox") == 0)
    {
        *mode = PREPROCESS_MODE_LETTERBOX;
        return true;
    }
    printf("unsupported --preprocess-mode '%s', expected resize or letterbox\n", value);
    return false;
}

void init_resize_preprocess_transform(preprocess_transform_t *transform,
                                      int src_width, int src_height,
                                      int model_width, int model_height)
{
    if (transform == NULL)
    {
        return;
    }
    memset(transform, 0, sizeof(*transform));
    transform->mode = PREPROCESS_MODE_RESIZE;
    transform->src_width = src_width;
    transform->src_height = src_height;
    transform->model_width = model_width;
    transform->model_height = model_height;
    transform->scale_x = src_width > 0 ? (float)model_width / (float)src_width : 0.0f;
    transform->scale_y = src_height > 0 ? (float)model_height / (float)src_height : 0.0f;
    transform->pad_x = 0.0f;
    transform->pad_y = 0.0f;
}
