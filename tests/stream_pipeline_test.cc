#include "stream_pipeline.h"

#include <assert.h>

#include <mutex>
#include <string>
#include <vector>

struct HookRecorder
{
    std::mutex mutex;
    std::vector<std::string> events;

    void add(const char *event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    }

    size_t count(const char *event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        size_t result = 0;
        for (size_t i = 0; i < events.size(); ++i)
        {
            if (events[i] == event)
            {
                ++result;
            }
        }
        return result;
    }
};

static StreamPipelineHooks make_hooks(HookRecorder *recorder, bool has_model)
{
    StreamPipelineHooks hooks;
    hooks.has_model = has_model;
    hooks.initialize_gstreamer = [recorder]() { recorder->add("initialize"); };
    hooks.inference_loop = [recorder]() { recorder->add("inference"); };
    hooks.rtsp_loop = [recorder]() { recorder->add("rtsp"); };
    hooks.stop_pipeline = [recorder]() { recorder->add("stop"); };
    hooks.notify_waiters = [recorder]() { recorder->add("notify"); };
    return hooks;
}

static void test_gstreamer_pipeline_without_model()
{
    HookRecorder recorder;
    StreamPipeline pipeline(make_hooks(&recorder, false));
    pipeline.start();
    pipeline.join();

    assert(recorder.count("initialize") == 1);
    assert(recorder.count("inference") == 0);
    assert(recorder.count("rtsp") == 1);
}

static void test_gstreamer_pipeline_with_model()
{
    HookRecorder recorder;
    StreamPipeline pipeline(make_hooks(&recorder, true));
    pipeline.start();
    pipeline.join();

    assert(recorder.count("initialize") == 1);
    assert(recorder.count("inference") == 1);
    assert(recorder.count("rtsp") == 1);
}

int main()
{
    test_gstreamer_pipeline_without_model();
    test_gstreamer_pipeline_with_model();
    return 0;
}
