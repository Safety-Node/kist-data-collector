# Storage format

**Recommendation: persist the compressed wire payloads verbatim, one blob
file + one CSV index per stream** (this is what's implemented):

```
sessions/<YYYYMMDD_HHMMSS>/
  meta.yaml               # start/end (UTC + epoch ns), endpoint, per-stream counters,
                          #   operator label `result: success|fail` (S/F keypress at
                          #   Ctrl-C stop; absent = unlabeled)
  lowstate.csv            # G1 rt/lowstate @~1 kHz, one row per message (if enabled):
                          #   recv_ns,tick,modes,IMU,per-motor q/dq/ddq/tau_est
  hand_left.csv           # Dex3 rt/dex3/<side>/state, one row per message (if enabled):
  hand_right.csv          #   recv_ns, 7 finger motors x q/dq/ddq/tau, pressure pads
  uwb.csv                 # UWB fixes, one row per rt/kist/uwb/pose (if enabled)
  lowcmd.csv              # body commands rt/lowcmd, one row per message (if enabled):
  arm_sdk.csv             #   recv_ns, modes, per-motor mode/q/dq/tau/kp/kd (arm_sdk: same type)
  hand_cmd_left.csv       # Dex3 rt/dex3/<side>/cmd, one row per message (if enabled):
  hand_cmd_right.csv      #   recv_ns, 7 finger motors x mode/q/dq/tau/kp/kd
  motion_token.csv        # gearsonic decoder tokens rt/kist/motion_token @50 Hz (if enabled):
                          #   recv_ns,stamp_ns,seq,mode tags,64-dim token
  <camera>/               # one dir per camera name (head, left_wrist, ...)
    color.h264            # Annex-B H.264 NAL units, appended frame by frame
    color.idx.csv         # seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size
    depth.rvl             # RVL bitstreams (lossless Z16), appended frame by frame
    depth.idx.csv         # seq,stamp_ns,recv_ns,width,height,depth_scale,offset,size
```

Row-shaped streams (robot state, hands, UWB) go straight to CSV — small
fixed-schema records don't need the blob+index form; both writer shapes
share the same bounded-queue hand-off (`common/record_queue.hpp`), so the
loss accounting below applies uniformly.

## CSV schemas

One row per DDS message; floats are `%.7g`. Each session's `meta.yaml`
carries the same description next to the data.

### Camera index CSVs — `color.idx.csv` / `depth.idx.csv` (30 fps)

| column | type | meaning |
|---|---|---|
| `seq` | uint64 | capture sequence from the transmitter — shared between a camera's color and depth, so it is the frame-pairing key |
| `stamp_ns` | int64 | capture time, transmitter clock (epoch ns) |
| `recv_ns` | int64 | arrival time on this host (epoch ns) — the cross-stream alignment column |
| `width`, `height` | int | frame size (px) |
| `is_keyframe` *(color only)* | 0/1 | IDR marker — decoding can start here |
| `depth_scale` *(depth only)* | float | meters per Z16 unit (0.001 D435, 0.0001 D405) |
| `offset`, `size` | uint64 | byte slice of this frame inside the blob file |

### `lowstate.csv` — 158 columns, ~1 kHz

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) |
| `tick` | 1 | robot-side ms counter |
| `mode_machine`, `mode_pr` | 2 | robot mode flags |
| `quat_w`..`quat_z` | 4 | IMU orientation quaternion (w, x, y, z) |
| `gyro_x`..`gyro_z` | 3 | angular velocity (rad/s) |
| `accel_x`..`accel_z` | 3 | linear acceleration (m/s²) |
| `rpy_roll`, `rpy_pitch`, `rpy_yaw` | 3 | Euler angles (rad) |
| `imu_temp` | 1 | IMU temperature (°C) |
| `m00_q` .. `m34_tau` | 140 | body motors 0-34 × (`q` rad, `dq` rad/s, `ddq`, `tau_est` Nm); unused slots read 0 |

### `hand_left.csv` / `hand_right.csv` — 137 columns, ~830 Hz

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) — the only timestamp (the hand message carries none) |
| `f0_q` .. `f6_tau` | 28 | finger motors 0-6 × (`q`, `dq`, `ddq`, `tau_est`); `f0` = thumb rotation (opposition), `f1`-`f2` thumb bend, `f3`-`f4` index, `f5`-`f6` middle |
| `press0_0` .. `press8_11` | 108 | fingertip press pads 0-8 × 12 pressure channels |

### `uwb.csv` — 5 columns, per received fix

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) |
| `stamp_ns` | 1 | publish time, transmitter clock (epoch ns) |
| `x`, `y`, `z` | 3 | position in the UWB local frame (m) |

The transmitter publishes valid fixes only and goes silent otherwise — a
time gap between rows means "no fix", not loss.

### `lowcmd.csv` / `arm_sdk.csv` — 213 columns, up to ~1 kHz

One row per `rt/lowcmd` / `rt/arm_sdk` message (same unitree_hg LowCmd
type). These are the **action** streams: what a controller told the robot
to do, next to the `lowstate.csv` observation.

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) — the only timestamp (commands carry no tick) |
| `mode_pr`, `mode_machine` | 2 | control-mode bytes echoed from the command |
| `m00_mode` .. `m34_kd` | 210 | body motors 0-34 × (`mode`, `q` rad — absolute joint target, `dq` rad/s, `tau` Nm feed-forward, `kp`, `kd` gains) |

A command topic is silent while no controller publishes on it — a time gap
means "no commands", not loss.

### `hand_cmd_left.csv` / `hand_cmd_right.csv` — 43 columns

One row per `rt/dex3/<side>/cmd` message — the action twin of
`hand_<side>.csv`.

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) |
| `f0_mode` .. `f6_kd` | 42 | finger motors 0-6 × (`mode`, `q`, `dq`, `tau`, `kp`, `kd`); `f0` = thumb rotation |

### `motion_token.csv` — 69 columns, 50 Hz

One row per `rt/kist/motion_token` message: the 64-dim SONIC token the
gearsonic whole-body decoder actually consumed that tick — the **training
action** next to the observations. It is not recomputable offline (the
encoder input is gearsonic's planner/teleop pipeline state, not robot
state), which is why gearsonic publishes it live.

| columns | n | meaning |
|---|---|---|
| `recv_ns` | 1 | arrival time on this host (epoch ns) |
| `stamp_ns` | 1 | gearsonic's computation-tick clock (epoch ns) — the 50 Hz alignment key |
| `seq` | 1 | +1 per decoded tick; gaps = robot outside CONTROL (INIT, damping, e-stop), not loss |
| `arbiter_mode` | 1 | 0 normal / 1 teleop / 2 vla / 3 recovering — the training export keeps `1` |
| `encoder_mode` | 1 | 0 g1 / 1 teleop / 255 = encoder skipped (vla, recovery) |
| `t00` .. `t63` | 64 | the token values |

## Cross-stream sync

Every file shares the `recv_ns` column — epoch ns on this host — so
`pandas.merge_asof(..., on="recv_ns")` aligns any pair of streams. Within
one camera, color and depth pair by `seq` instead (exact capture pairing).
