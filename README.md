# rknn_detect

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-RK3568%20%7C%20RK3588-orange.svg)](#硬件与依赖)

面向 Rockchip RK3568/RK3588 的嵌入式目标检测与智能安防参考项目。当前主线以
`YOLOv6n + RKNN + GStreamer + H.264/RTSP` 为实时链路：开发板负责采集、推理和
分析数据发布，主机端 Host Viewer 负责 RTSP 播放、检测/轨迹叠加、区域入侵可视化、
录制和回放。

## 当前能力

实时链路如下：

```text
摄像头
  │
  ▼
V4L2 ──> GStreamer tee ──> mpph264enc ──> H.264 RTSP ──> Host Viewer
                     │
                     └──> appsink ──> RKNN YOLOv6n ──> Tracker/Event ──> HTTP JSON
                                                                          │
                                                                          └─> Host overlay
```

## 硬件与依赖

| 位置 | 要求 |
| --- | --- |
| 开发板 | RK3568 已验证；RK3588 目标需要自行准备匹配的 SDK、sysroot、工具链和构建命令 |
| 系统 | 目标为嵌入式 Linux/aarch64；当前开发板验证环境为 Debian 11 |
| 摄像头 | 可用的 V4L2 设备，默认 `/dev/video0`，默认格式为 NV12、640×480、30 FPS |
| 板端运行库 | RKNN Runtime、RGA、GStreamer 1.x、`v4l2src`、Rockchip `mpph264enc` 和 RTSP 相关运行库 |
| 主机编译 | CMake 3.10+、GNU Make、aarch64 交叉编译器、对应的 GStreamer 开发 sysroot |
| Rockchip SDK | RKNPU2 SDK 1.3.0 或与模型/runtime 匹配的版本；CMake 需要 SDK 中的 RKNN、OpenCV 和 RGA |
| Host Viewer | Python 3.9+；GUI 需要 PySide6 和带 RTSP 支持的 OpenCV |

### 模型资源

出于体积、来源和许可证边界，除 RK3568 示例模型外，以下内容由 `.gitignore` 排除，不会
随着 `git clone` 自动下载：

- 其他 `*.rknn`、`*.onnx`、训练权重和完整数据集；
- 交叉编译工具链、GStreamer sysroot 和 Rockchip RKNPU2 SDK；
- 运行时录制 Session、视频、日志和构建目录。

仓库保留 `model/RK356X/yolov6n-640-640.rknn` 作为 RK3568 示例模型；其他模型资源仍
需自行取得并确认许可证。`model/bus.jpg`、COCO 标签文件、配置 Profile、JSON Schema
和转换脚本可以直接用于示例流程。

## 快速开始

### 1. 准备 RKNN 模型

将已经转换好的 RK3568 YOLOv6n 模型放到构建树中。文件名可以自定义，但后续命令和
Host Viewer 的 `--model-path` 必须使用同一个路径：

```bash
mkdir -p model/RK356X
cp /path/to/yolov6n-640-640.rknn model/RK356X/yolov6n-640-640.rknn
test -f model/RK356X/yolov6n-640-640.rknn
```

模型必须与当前代码的 YOLOv6 输出布局、类别数和输入尺寸契约一致；不能只把任意
YOLOv5 或其他 YOLOv6 变体模型改名后直接使用。

如果需要自行转换模型，先安装与目标 RKNN Runtime 匹配的 RKNN Toolkit2，并准备
YOLOv6n ONNX 文件和量化校准图片列表：

```bash
cd convert_rknn_demo/yolov6n
python onnx2rknn.py \
  --onnx /path/to/yolov6n.onnx \
  --dataset /path/to/calibration.txt \
  --output ../../model/RK356X/yolov6n-640-640.rknn \
  --target-platform rk3568
cd ../..
```

### 2. 交叉编译

先准备以下目录：

```text
RKNPU2_ROOT/
├── runtime/RK356X/Linux/librknn_api/
└── examples/3rdparty/
    ├── opencv/opencv-linux-aarch64/
    └── rga/RK356X/

GST_SYSROOT/
├── usr/lib/aarch64-linux-gnu/pkgconfig/
└── usr/share/pkgconfig/
```

RK3568 构建：

```bash
export RKNPU2_ROOT=/opt/rockchip/rknpu2_1.3.0
export GST_SYSROOT=/opt/sysroots/rk3568-bullseye
export CROSS_COMPILE=/opt/toolchains/aarch64-linux-gnu

./build-linux_RK356X.sh
```

如果使用脚本默认的 Linaro 工具链布局，也可以设置 `TOOL_CHAIN`，脚本会在其
`bin/aarch64-linux-gnu-*` 下寻找编译器：

```bash
export TOOL_CHAIN=/opt/toolchains/gcc-linaro-aarch64
./build-linux_RK356X.sh
```

脚本构建产物在 `install/rknn_detect_Linux/`，其中包括：

```text
rknn_detect_demo
rknn_detect_stream_server
rga_resize_probe
rga_rknn_mem_probe
lib/
model/                         # 示例图片、标签和公开的 RK356X 示例模型
```

如果使用已有的 CMake 构建目录且更换了 SDK 或 sysroot，建议先删除本地生成的
`build/` 后重新配置，避免 CMake 缓存旧路径。

### 3. 部署到开发板

开发板需要先安装或自带 RKNN Runtime、GStreamer 运行库和 Rockchip MPP GStreamer
插件。部署前检查关键元素：

```bash
ssh root@<your IP> 'gst-inspect-1.0 v4l2src mpph264enc'
```

将安装目录同步到开发板（把 IP 改成自己的板端地址）：

```bash
rsync -avz install/rknn_detect_Linux/ \
  root@<your IP>:/root/project/rknn_detect/
```

板端启动实时服务：

```bash
ssh root@<your IP>
cd /root/project/rknn_detect
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

./rknn_detect_stream_server \
  --port 18080 \
  --rtsp-port 8554 \
  --rtsp-path /rknn_detect \
  --video-node /dev/video0 \
  model/RK356X/yolov6n-640-640.rknn
```

没有物理显示屏也可以运行，`--display-mode none` 是默认值。若摄像头不是 `/dev/video0`
或分辨率/格式不同，请根据板端 V4L2 能力调整 `--video-node`、`--width`、`--height`、
`--fps` 和 `--pixel-format`。

### 4. 检查 HTTP/RTSP 输出

```bash
curl http://<your IP>:18080/healthz
curl http://<your IP>:18080/status.json
curl http://<your IP>:18080/detections.json
```

RTSP 地址为：

```text
rtsp://<your IP>:8554/rknn_detect
```

可以使用 `ffplay`、VLC 或其他支持 H.264 RTSP 的播放器验证视频：

```bash
ffplay -rtsp_transport tcp rtsp://<your IP>:8554/rknn_detect
```

直接运行 `rknn_detect_demo` 做单图片推理：

```bash
cd /root/project/rknn_detect
./rknn_detect_demo \
  model/RK356X/yolov6n-640-640.rknn \
  model/bus.jpg
```

## Host Viewer

Host Viewer 通过 SSH 启停板端进程，同时从 RTSP 和 HTTP 旁路读取视频及分析元数据。
它不要求在开发板安装 PySide6。

### 安装主机依赖

```bash
python3 -m venv .venv-host
source .venv-host/bin/activate
python -m pip install --upgrade pip
python -m pip install PySide6 opencv-python
```

如果只使用 CLI 的 SSH 控制和 `--dry-run`，系统 Python 3.9+ 即可；GUI 播放 RTSP 时，
OpenCV 的构建还必须包含可用的 FFmpeg 或 GStreamer 后端。

### CLI 控制

先确认 SSH：

```bash
python -m tools.host_viewer.cli \
  --check-ssh \
  --board-host <your IP> \
  --board-user root \
  --remote-project-dir /root/project/rknn_detect
```

打印即将执行的板端命令，不会启动进程：

```bash
python -m tools.host_viewer.cli \
  --dry-run \
  --board-host <your IP> \
  --model-path model/RK356X/yolov6n-640-640.rknn
```

启动、查看状态和停止：

```bash
python -m tools.host_viewer.cli --start --board-host <your IP>
python -m tools.host_viewer.cli --status --board-host <your IP>
python -m tools.host_viewer.cli --stop --board-host <your IP>
```

### GUI

```bash
python -m tools.host_viewer.gui \
  --board-host <your IP> \
  --board-user root \
  --remote-project-dir /root/project/rknn_detect \
  --model-path model/RK356X/yolov6n-640-640.rknn
```

Live 工作区负责实时运行，启动后会校验板端状态、RTSP 帧和对齐的检测元数据；Replay
工作区只读取本地主机上的完整录制 Session，不会启动板端进程。录制模式包括：

- `Off`：不创建录制 Session；
- `Metadata-only`：只保存检测、观察和轨迹分析数据；
- `Sampled JPEG`：按间隔保存部分 JPEG；
- `All JPEG`：尝试为每条分析记录保存 JPEG，适合短时诊断。

场景 Profile 在 `profiles/tracking/`，摄像头区域在 `profiles/cameras/`，区域事件规则
在 `profiles/events/`。这些配置在 Live 启动前解析并生成 Profile Hash，运行中不会热更新。

Tracker 重算和策略 + Tracker 重算还需要构建主机端 `tracking_replay`：

```bash
cmake -S tools/tracking_replay -B build/tracking_replay
cmake --build build/tracking_replay -j2
export PATH="$PWD/build/tracking_replay:$PATH"
```

## HTTP 接口

默认直接启动服务时 HTTP 端口为 `8080`；本文部署命令和 Host Viewer 使用 `18080`。
如果修改了 `--port`，请同步修改 Host Viewer 的 `--stream-port`。

| 接口 | 用途 |
| --- | --- |
| `/healthz` | 轻量健康检查 |
| `/status.json` | 摄像头、RTSP、AI、性能和 Profile 状态 |
| `/detections.json` | 最新检测结果 |
| `/tracks.json` | 最新轨迹结果 |
| `/events.json` | 区域入侵事件和事件能力状态 |
| `/recording.json` | 录制 Session 状态 |
| `/snapshot.jpg` | 最新帧 JPEG 快照 |

检测 JSON 的稳定核心字段是 `capture_frame_id`、`capture_timestamp_ms` 和 `objects`。
接口返回的调试字段可能随版本增加；客户端应优先依赖上述稳定字段和 `status.json` 中
明确标注的版本信息。

## 测试与校验

不需要板端 SDK 的主机侧校验：

```bash
python3 tools/validate_yolov6_postprocess.py

cmake -S tests -B /tmp/rknn_detect_host_tests
cmake --build /tmp/rknn_detect_host_tests -j2
ctest --test-dir /tmp/rknn_detect_host_tests --output-on-failure
```

安装了 PySide6 后，可继续运行 Host Viewer 的 Python 测试：

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
```

板端验收还应单独确认摄像头节点、`mpph264enc`、RTSP 播放、HTTP 元数据和 NPU 推理，
主机测试通过不能替代真实硬件验证。

## 目录结构

```text
include/                 C++ 公共头文件
src/                     单图片与板端流服务实现
profiles/                Tracking、摄像头 ROI 和事件 Profile
schemas/                 录制/回放/配置 JSON Schema
convert_rknn_demo/       YOLOv6n 到 RKNN 的转换入口
tools/host_viewer/       主机端 CLI、GUI、录制与回放
tools/tracking_replay/   主机端 Tracker/Policy 重算工具
tests/                   C++ 和 Python 主机测试
third_party/             ByteTrack、Eigen、nlohmann/json 及其许可证
model/                   示例图片/标签和公开的 RK356X 示例模型
```

## 许可证

本项目自有代码采用 [MIT License](LICENSE)。`third_party/` 下的依赖保留各自的版权
声明和许可证；部分源文件包含 Rockchip 示例代码的 Apache 2.0 声明。RKNPU2 SDK、
GStreamer/系统组件、模型权重、训练数据和用户自行取得的其他资源不由本仓库许可证
重新授权，请分别遵守其上游许可证和使用条款。
