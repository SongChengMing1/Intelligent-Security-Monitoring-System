# RK3568 Smart Monitor Host Viewer

This directory contains the host-side upper-computer tooling for controlling and viewing the RK3568 smart-monitor stream.

## Host dependencies

Run the commands from the repository root. Create a host-only virtual environment if needed:

```bash
python3 -m venv .venv-host
source .venv-host/bin/activate
python -m pip install --upgrade pip
python -m pip install PySide6 opencv-python
```

PySide6 is a host-side dependency only. It is not required on the RK3568 board.

## Foundation Smoke Commands

Print the remote GStreamer stream command without launching it:

```bash
python -m tools.host_viewer.cli --dry-run
```

Start a bounded board stream and wait for completion:

```bash
python -m tools.host_viewer.cli --start --wait --max-seconds 5
```

Check the remote stream server:

```bash
python -m tools.host_viewer.cli --status
```

Request a clean stop for a stale viewer-managed stream process:

```bash
python -m tools.host_viewer.cli --stop
```

Start the desktop viewer:

```bash
python -m tools.host_viewer.gui \
  --board-host <your-ip> \
  --board-user root \
  --remote-project-dir /root/project/rknn_detect \
  --model-path model/RK356X/yolov6n-640-640.rknn
```

The GUI has only two normal workspaces:

- `Live`: `Start` performs SSH, Profile/ROI, recording and board launch checks. `Stop` ends the Live session; when recording is enabled it confirms, waits for recorder finalization, and copies the complete remote Session into the project `recordings/<Recording-ID>` directory. Live states are `Idle`, `Starting`, `Running`, `Degraded`, `Stopping` and `Error`.
- `Replay`: `Load Replay` opens a complete local Session paused at its first frame. `Play`, `Pause`, `Previous`, `Next` and `Stop` control the local session. The modes are `recorded`, `tracker-rerun` and `policy-tracker-rerun`; there is intentionally no speed selector in this version.

Scene Profiles are loaded from `profiles/tracking/`, while the logical `camera0` ROI is loaded from `profiles/cameras/camera0.json`. The GUI displays the resolved Profile Hash and locks scene, ROI and recording settings while Live is active. The four recording choices are `Off`, `Metadata-only`, `Sampled JPEG` and `All JPEG`.

Live uses the H.264 RTSP path and polls status, detections, tracks and recorder diagnostics independently. Running is declared only after board status, an RTSP frame and aligned detection metadata are present. The RTSP video stays clean; detector overlays are rendered by the host from the AI side channel. HTTP MJPEG is not supported.

`Display Mode` defaults to `none`, so no physical screen is required. Use `fakesink` only to validate the non-blocking local-display placeholder branch before real panel support is installed.

The GUI defaults to `Rotate = 0 deg`. Rotation changes only host-side display orientation.

For a recording-enabled Live run, the board writes to `/root/project/rknn_detect/recordings/<Recording-ID>`. After a successful Stop, the GUI shows the local Session path and pre-fills Replay's `Local Session` field. The copied Session directory is ignored by Git because it is runtime evidence.
