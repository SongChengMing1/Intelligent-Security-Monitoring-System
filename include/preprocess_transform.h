#ifndef RKNN_DETECT_PREPROCESS_TRANSFORM_H_
#define RKNN_DETECT_PREPROCESS_TRANSFORM_H_

typedef enum _preprocess_mode_t
{
    PREPROCESS_MODE_RESIZE = 0,
    PREPROCESS_MODE_LETTERBOX = 1,
} preprocess_mode_t;

typedef struct _preprocess_transform_t
{
    preprocess_mode_t mode;
    int src_width;
    int src_height;
    int model_width;
    int model_height;
    float scale_x;
    float scale_y;
    float pad_x;
    float pad_y;
} preprocess_transform_t;

const char *preprocess_mode_to_string(preprocess_mode_t mode);
bool parse_preprocess_mode(const char *value, preprocess_mode_t *mode);
void init_resize_preprocess_transform(preprocess_transform_t *transform,
                                      int src_width, int src_height,
                                      int model_width, int model_height);

#endif
