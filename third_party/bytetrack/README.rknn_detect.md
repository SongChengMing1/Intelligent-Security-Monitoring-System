# ByteTrack third-party source

This directory contains the detector-independent C++ tracking core adapted from:

```text
Repository: https://github.com/FoundationVision/ByteTrack.git
Revision: d1bf0191adff59bc8fcfeaa0b33d3d1642552a99
Original path: deploy/ncnn/cpp
License: MIT, see LICENSE
```

The ncnn/YOLOX detector demo is intentionally excluded. Local changes are limited to integration concerns:

- remove the ncnn demo's OpenCV rectangle/color dependency from the tracker input;
- expose configurable low/high/new-track and association thresholds;
- expose configured lost-step retention and state counts;
- drop observations below the configured low threshold;
- replace process-terminating assignment errors with C++ exceptions;
- correct LAPJV temporary allocation sizes.

Timestamp handling, class isolation, public IDs, lifecycle metadata, capacity limits, and error isolation are implemented by the project adapter outside this directory.
