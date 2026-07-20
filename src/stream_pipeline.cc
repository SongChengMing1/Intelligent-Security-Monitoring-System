#include "stream_pipeline.h"

#include <stdexcept>
#include <thread>

class StreamPipelineThreads
{
public:
    std::thread inference;
    std::thread rtsp;
};

StreamPipeline::StreamPipeline(const StreamPipelineHooks &hooks)
    : hooks_(hooks), threads_(new StreamPipelineThreads()), stop_requested_(false)
{
}

StreamPipeline::~StreamPipeline()
{
    request_stop();
    join();
}

void StreamPipeline::request_stop()
{
    if (stop_requested_)
    {
        return;
    }
    stop_requested_ = true;
    if (hooks_.stop_pipeline)
    {
        hooks_.stop_pipeline();
    }
    if (hooks_.notify_waiters)
    {
        hooks_.notify_waiters();
    }
}

void StreamPipeline::join()
{
    if (threads_->inference.joinable())
    {
        threads_->inference.join();
    }
    if (threads_->rtsp.joinable())
    {
        threads_->rtsp.join();
    }
}

void StreamPipeline::start_rtsp_thread()
{
    if (!hooks_.rtsp_loop)
    {
        throw std::logic_error("stream pipeline requires an RTSP loop");
    }
    threads_->rtsp = std::thread(hooks_.rtsp_loop);
}

void StreamPipeline::start_inference_thread()
{
    if (!hooks_.inference_loop)
    {
        throw std::logic_error("stream pipeline requires an inference loop");
    }
    threads_->inference = std::thread(hooks_.inference_loop);
}

const StreamPipelineHooks &StreamPipeline::hooks() const
{
    return hooks_;
}

void StreamPipeline::start()
{
    if (!hooks_.initialize_gstreamer)
    {
        throw std::logic_error("GStreamer stream pipeline requires initialization");
    }
    hooks_.initialize_gstreamer();
    if (hooks_.has_model)
    {
        start_inference_thread();
    }
    start_rtsp_thread();
}
