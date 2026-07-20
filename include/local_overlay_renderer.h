#ifndef LOCAL_OVERLAY_RENDERER_H
#define LOCAL_OVERLAY_RENDERER_H

#include <string>
#include <vector>

#include "stream_overlay_policy.h"

struct LocalOverlayRenderFrame
{
    long long video_frame_id;
    long long video_time_ms;
    int source_width;
    int source_height;
    OverlaySelection selection;

    LocalOverlayRenderFrame()
        : video_frame_id(0),
          video_time_ms(0),
          source_width(0),
          source_height(0),
          selection()
    {
    }
};

class LocalOverlayRenderer
{
public:
    virtual ~LocalOverlayRenderer() {}
    virtual const char *name() const = 0;
    virtual bool render(const LocalOverlayRenderFrame &frame) = 0;
};

class NullLocalOverlayRenderer : public LocalOverlayRenderer
{
public:
    const char *name() const { return "noop"; }
    bool render(const LocalOverlayRenderFrame &frame)
    {
        (void)frame;
        return true;
    }
};

#endif // LOCAL_OVERLAY_RENDERER_H
