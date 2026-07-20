# Eigen third-party source

This directory vendors Eigen as a header-only dependency for the ByteTrack Kalman filter:

```text
Repository: https://gitlab.com/libeigen/eigen.git
Tag: 3.3.9
Revision: 0fd6b4f71dd85b2009ee4d1aeb296e2c11fc9d68
```

The build defines `EIGEN_MPL2_ONLY` so including code that requires Eigen's LGPL components fails at compile time. License texts and upstream `COPYING.README` are retained in this directory.
