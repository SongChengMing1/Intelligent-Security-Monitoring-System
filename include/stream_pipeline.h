#ifndef RKNN_DETECT_STREAM_PIPELINE_H_
#define RKNN_DETECT_STREAM_PIPELINE_H_

#include <functional>
#include <memory>

// The application supplies work-specific hooks; this module owns which hooks
// run for a pipeline and the threads that execute them.
struct StreamPipelineHooks
{
    std::function<void()> initialize_gstreamer;
    std::function<void()> inference_loop;
    std::function<void()> rtsp_loop;
    std::function<void()> stop_pipeline;
    std::function<void()> notify_waiters;
    bool has_model;

    StreamPipelineHooks()
        : initialize_gstreamer(),
          inference_loop(),
          rtsp_loop(),
          stop_pipeline(),
          notify_waiters(),
          has_model(false)
    {
    }
};

class StreamPipeline
{
public:
    explicit StreamPipeline(const StreamPipelineHooks &hooks);
    virtual ~StreamPipeline();

    void start();
    void request_stop();
    void join();

protected:
    void start_rtsp_thread();
    void start_inference_thread();
    const StreamPipelineHooks &hooks() const;

private:
    StreamPipeline(const StreamPipeline &);
    StreamPipeline &operator=(const StreamPipeline &);

    StreamPipelineHooks hooks_;
    std::unique_ptr<class StreamPipelineThreads> threads_;
    bool stop_requested_;
};

#endif  // RKNN_DETECT_STREAM_PIPELINE_H_
