#ifndef RKNN_DETECT_PERF_STATS_H_
#define RKNN_DETECT_PERF_STATS_H_

#include <sys/time.h>

typedef struct _perf_stats_t
{
    double capture_ms;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    double render_ms;
    double total_ms;
    double inference_fps;
    double e2e_fps;
    int captured_frames;
    int processed_frames;
    int dropped_frames;
} perf_stats_t;

double elapsed_ms(struct timeval start, struct timeval stop);
void update_perf_totals(perf_stats_t *stats);
void print_perf_stats(const perf_stats_t *stats);

#endif
