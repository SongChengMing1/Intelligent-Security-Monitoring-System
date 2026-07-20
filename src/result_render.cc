#include "result_render.h"

#include <stdio.h>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

void render_and_save_results_to_path(cv::Mat *orig_img, const detect_result_group_t *detect_result_group,
                                     bool save_result, const char *output_path, perf_stats_t *perf_stats)
{
    struct timeval stage_start_time, stage_stop_time;
    char text[256];

    gettimeofday(&stage_start_time, NULL);
    for (int i = 0; i < detect_result_group->count; i++)
    {
        const detect_result_t *det_result = &(detect_result_group->results[i]);
        sprintf(text, "%s %.1f%%", det_result->name, det_result->prop * 100);
        printf("%s @ (%d %d %d %d) %f\n",
               det_result->name,
               det_result->box.left, det_result->box.top, det_result->box.right, det_result->box.bottom,
               det_result->prop);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        rectangle(*orig_img, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255, 0, 0, 255), 3);
        putText(*orig_img, text, cv::Point(x1, y1 + 12), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
    }

    if (save_result)
    {
        imwrite(output_path && output_path[0] != '\0' ? output_path : "./out.jpg", *orig_img);
    }
    gettimeofday(&stage_stop_time, NULL);
    perf_stats->render_ms = elapsed_ms(stage_start_time, stage_stop_time);
}

void render_and_save_results(cv::Mat *orig_img, const detect_result_group_t *detect_result_group,
                             bool save_result, perf_stats_t *perf_stats)
{
    render_and_save_results_to_path(orig_img, detect_result_group, save_result, "./out.jpg", perf_stats);
}
