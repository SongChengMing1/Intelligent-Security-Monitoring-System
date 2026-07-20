#include "perf_stats.h"

#include <stdio.h>

static double perf_get_us(struct timeval t)
{
    return (t.tv_sec * 1000000 + t.tv_usec);
}

double elapsed_ms(struct timeval start, struct timeval stop)
{
    return (perf_get_us(stop) - perf_get_us(start)) / 1000.0;
}

void update_perf_totals(perf_stats_t *stats)
{
    stats->total_ms = stats->capture_ms + stats->preprocess_ms + stats->inference_ms + stats->postprocess_ms + stats->render_ms;
    stats->inference_fps = stats->inference_ms > 0.0 ? 1000.0 / stats->inference_ms : 0.0;
    stats->e2e_fps = stats->total_ms > 0.0 ? 1000.0 / stats->total_ms : 0.0;
}

void print_perf_stats(const perf_stats_t *stats)
{
    printf("perf stats: capture=%.3f ms, preprocess=%.3f ms, inference=%.3f ms, postprocess=%.3f ms, render=%.3f ms, total=%.3f ms, inference_fps=%.2f, e2e_fps=%.2f, captured=%d, processed=%d, dropped=%d\n",
           stats->capture_ms, stats->preprocess_ms, stats->inference_ms, stats->postprocess_ms,
           stats->render_ms, stats->total_ms, stats->inference_fps, stats->e2e_fps,
           stats->captured_frames, stats->processed_frames, stats->dropped_frames);
}
