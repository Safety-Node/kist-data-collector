# Storage format

**Recommendation: persist the compressed wire payloads verbatim, one blob
file + one CSV index per stream** (this is what's implemented):

```
sessions/<YYYYMMDD_HHMMSS>/
  meta.yaml               # start/end (UTC + epoch ns), endpoint, per-stream counters
  lowstate.csv            # G1 rt/lowstate @~1 kHz, one row per message (if enabled):
                          #   recv_ns,tick,modes,IMU,per-motor q/dq/ddq/tau_est
  hand_left.csv           # Dex3 rt/dex3/<side>/state, one row per message (if enabled):
  hand_right.csv          #   recv_ns, 7 finger motors x q/dq/ddq/tau, pressure pads
  <camera>/               # one dir per camera name (head, left_wrist, ...)
    color.h264            # Annex-B H.264 NAL units, appended frame by frame
    color.idx.csv         # seq,stamp_ns,recv_ns,width,height,is_keyframe,offset,size
    depth.rvl             # RVL bitstreams (lossless Z16), appended frame by frame
    depth.idx.csv         # seq,stamp_ns,recv_ns,width,height,depth_scale,offset,size
```

Row-shaped streams (robot state now, UWB later) go straight to CSV — small
fixed-schema records don't need the blob+index form; both writer shapes
share the same bounded-queue hand-off (`common/record_queue.hpp`), so the
loss accounting below applies uniformly.

CSV schemas (one row per DDS message; floats are `%.7g`; each session's
`meta.yaml` carries the same description next to the data):

| file | columns |
|---|---|
| `lowstate.csv` (158 cols) | `recv_ns`, `tick` (robot ms counter), `mode_machine`, `mode_pr`, IMU (`quat_w..z`, `gyro_x..z`, `accel_x..z`, `rpy_*`, `imu_temp`), body motors `m00..m34` × `q/dq/ddq/tau` |
| `hand_left.csv` / `hand_right.csv` (65 cols) | `recv_ns`, finger motors `f0..f6` × `q/dq/ddq/tau` (`f0` = thumb rotation), press pads `press0..2` × 12 pressure channels |

Cross-stream sync: every file (camera indices included) shares the `recv_ns`
column — epoch ns on this host — so `pandas.merge_asof(..., on="recv_ns")`
aligns any pair of streams.

- `stamp_ns` — transmitter capture clock (epoch ns); `recv_ns` — this host's
  arrival clock (epoch ns), the column that aligns cameras with every other
  stream recorded on this machine. `offset,size` slice the frame out of the
  blob, so the index is both the timestamp record and the random-access map.

Why this format:

1. **Losslessness is structural.** No decode/re-encode on the hot path means
   the write rate equals the wire rate (~0.5 MB/s color + ~3.5 MB/s depth per
   camera, measured) — a queue in front of a plain `write()` at that rate
   cannot back up on any sane disk, so `dropped` stays 0 by construction.
   Decoding to per-frame images instead (180 decodes + image encodes/s for
   3 cameras) turns the recorder CPU-bound and makes loss a tuning question.
2. **Byte-exact fidelity.** What's on disk is bit-for-bit what the wire
   carried — the recorder never touches the payload bytes. Depth (RVL) is
   lossless Z16; color keeps exactly the H.264 the robot actually streamed.
3. **Directly consumable.** `color.h264` is a standard elementary stream —
   ffmpeg/mpv/OpenCV read it as-is (see the README's Exporting section). Depth decodes with the
   RVL decoder already in the vendored snapshot (`RvlDepthDecoder`).
4. **Cheap on the storage processor.** ~4 MB/s per camera measured
   (color 0.5 + depth 3.5) — ~44 GB/hour for 3 cameras, a few percent
   CPU total. Depth dominates; RVL is ~5× over raw Z16, lossless.

Alternatives considered:

- **Decoded per-frame PNGs** — friendliest to browse, but ~10× the write
  bandwidth for color, heavy CPU, and loss becomes possible under load.
  Better done offline from this format (and then it's reproducible).
- **MCAP / rosbag2** — standard robotics containers, nice tooling; adds a
  dependency plus schema registration for the custom `kist_msgs` types, and
  readers still need ffmpeg/RVL to see pixels. Worth revisiting if many
  low-rate streams join and per-stream files get unwieldy; the blob+index
  layout converts to MCAP losslessly at any time.
