# kist-data-collector

Storage-side recorder for the KIST G1 stack — runs on the data-storage
processor next to the control processor, subscribes to the DDS streams
published by [kist-ext-sensor-io](https://github.com/Safety-Node/kist-ext-sensor-io),
and persists them **without losing frames**. Camera streams (the hard case:
3 cameras × color + depth × 30 fps) are implemented first; other streams
(UWB, robot state) slot into the same session layout later.

## Why not read the DataBuffer?

kist-ext-sensor-io hands consumers a latest-wins `DataBuffer` — writers
overwrite, readers snapshot. That contract is right for control/inference
(only the newest frame matters) and wrong for recording: any poll that lands
after two `SetData()` calls has already lost a frame, at every polling rate.

The recorder therefore taps the subscribers' public `set_on_frame()` hook
instead, which fires on the DDS receive thread once per delivered message:

```
DDS topic ─> ColorSubscriber ──on_frame──> bounded queue ─> writer thread ─> color.h264 + color.idx.csv
DDS topic ─> DepthSubscriber ──on_frame──> bounded queue ─> writer thread ─> depth.rvl  + depth.idx.csv
             (kist-ext-sensor-io,          (copy only,      (append verbatim
              unmodified)                   never blocks)     + CSV index, flush)
```

The hand-off is a synchronous call chain on the receive thread (the frame is
queued in the same call that writes the buffer), so buffer→queue loss is
impossible by construction. The remaining loss surfaces are all counted, per
stream, live at 1 Hz and in the session summary:

- `dropped` — frames the recorder's queue refused (disk stalled longer than
  `queue_capacity`/fps seconds).
- `write_errors` — frames the filesystem refused (disk full, I/O error).
- `wire_gaps` — holes in the publisher's `seq` before arrival (Tx→Rx loss).
  Recorded so post-hoc analysis knows; additionally the camera readers
  request RELIABLE QoS (`realsense_cameras.reliable`, default true) so
  isolated wire losses are recovered by RTPS retransmission — the writer's
  shallow history (keep-last-1, one frame period) bounds the recovery
  window, so sustained congestion can still gap.

**`dropped` and `write_errors` both 0 means every frame that reached this
process is on disk.**

## Storage format

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
   ffmpeg/mpv/OpenCV read it as-is (see Exporting). Depth decodes with the
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

## Build

Docker (recommended — bakes idlc toolchain + unitree_sdk2 + build):

```bash
./docker/build.sh      # builds the kist-data-collector image
./docker/run.sh        # shell in the container; binaries under build/
                       # recordings land in $SESSIONS_DIR (default ~/kist-data-collector-sessions)
```

The image clones `kist-ext-sensor-io` (and `unitree_sdk2` inside it) at
build time, pinned to validated commits — bump the `EXT_SENSOR_IO_COMMIT`
build-arg to move to a newer upstream. The upstream is used unmodified (the
recorder only links its public libraries); if patching it ever becomes
necessary, switch back to committing a vendored snapshot instead of
patching the clone, so image builds stay reproducible.

Manual (non-Docker) build: install the CycloneDDS 0.10.2 idlc toolchain +
yaml-cpp (kist-ext-sensor-io's README has the steps), clone the pinned
thirdparty repos into place, then `cmake -B build && cmake --build build`:

```bash
git clone https://github.com/Safety-Node/kist-ext-sensor-io.git thirdparty/kist-ext-sensor-io
git -C thirdparty/kist-ext-sensor-io checkout d97b554b6ac898e1cea5e478a467bf3e8357766c

git clone https://github.com/unitreerobotics/unitree_sdk2.git \
    thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2
git -C thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2 \
    checkout 21d0a3b2c46ee48c8fdf2783becb6be3beb0a59b
```

## Usage

```bash
./build/kist_data_collector [config/config.yaml]
```

Every stream section in `config/config.yaml` follows the same pattern —
`enabled` + `queue_capacity`, and for `realsense_cameras` a `cameras:` list
whose entries may carry their own `enabled` (default true; the `name` must
match the transmitter's camera names). The collector records every enabled
stream into one session dir, prints per-second per-stream
`rx/wr fps, drop, gap, MB`, and on Ctrl-C drains the queues, appends the
per-stream counters to `meta.yaml`, and exits.

## Deployment tuning (robot LAN)

**A subtlety worth knowing first**: the unitree SDK's
`ChannelFactory::Init(domain, iface)` builds its own DDS config to select
the network interface and creates the domain from it — which makes CycloneDDS
**silently ignore `CYCLONEDDS_URI`**. Any tuning delivered via that env
never reaches the sockets (measured: buffers stay at Cyclone's 1 MiB
default). The collector therefore keeps ALL transport config —
**the network interface and the receive tuning** — in
`config/cyclonedds.xml` (path set by `unitree.dds_config`), routes it via
`CYCLONEDDS_URI` itself, and passes the SDK an empty interface so the file
actually applies. This is validated loudly at startup (missing file =
fatal; the effective URI is printed) because the failure mode is silent.
To change the NIC, edit the `NetworkInterface name=` in the XML — not
config.yaml. (Verified: sockets at 32 MB, traffic flowing.)

Three cameras' depth frames (~190 KB RVL each) arrive nearly simultaneously;
Linux's default UDP receive buffer overflows on those bursts and sheds
fragments — measured as depth `wire_gaps`. The buffer request above still
needs the host kernel to allow it — once per storage machine:

```bash
sudo tee /etc/sysctl.d/99-dds-buffers.conf <<'EOF'
net.core.rmem_max = 134217728
EOF
sudo sysctl --system
```

(`rmem_max` is only a permission ceiling — no memory is used unless a socket
asks, and this image's CycloneDDS is the one asking. `rmem_default` is left
alone since the request is explicit.)

The **sender** (the machine running kist-ext-sensor-io's transmitter) wants
the mirror-image tuning (`net.core.wmem_max` + `SocketSendBufferSize`), and
it has the same SDK env-override problem: a `CYCLONEDDS_URI` export is
ignored while the transmitter passes a non-empty interface. Until
kist-ext-sensor-io builds its URI the way this repo does, the code-free
workaround on the sender is: set `network_interface: ""` in its config.yaml
and put BOTH the interface and the send tuning into the env, e.g.
`CYCLONEDDS_URI='<CycloneDDS><Domain id="any"><General><Interfaces>
<NetworkInterface name="eth0"/></Interfaces></General><Internal>
<SocketSendBufferSize min="16MB"/></Internal></Domain></CycloneDDS>'`.

Loss ledger from the robot-LAN measurements (2026-07-31): receiver rcvbuf
overflow was the dominant loss (~0.9% of depth) until the sysctl; sender
sndbuf the next (~0.4%) until the tuning above; what remains is ~0.1%,
bursty 1-4 frame transit losses. Publish-vs-receive accounting showed the
x264 encoder skips almost nothing (color/depth publish counts match within
0-2 per 2 min), and the recorder side stayed lossless throughout every run
(dropped / write_errors 0; lowstate 96,975/96,975 @ ~1 kHz). The residual
is best handled at dataset-build time: gate episodes on their per-stream
wire_gap counters (meta.yaml) rather than chasing zero on a best-effort
transport.

## Verifying a session

```bash
python3 scripts/verify_session.py sessions/<stamp>
```

Checks every stream's index against its blob (seq contiguity, offset/size
chain, file size) and reports wire gaps; combine with the `dropped` /
`write_errors` counters in `meta.yaml` for the full losslessness picture.
(The pipeline was originally validated byte-for-byte against a deterministic
fake transmitter — 3 cameras × 300 frames, every payload byte reproduced;
that harness has since been removed to keep the repo minimal.)

## Live record & playback walkthrough (same-machine, one camera)

Everything below runs **inside the collector container** — `docker/run.sh`
wires host networking (DDS), the X11 socket (ffplay windows), and the
`sessions/` mount (recordings persist on the host at `$SESSIONS_DIR`,
default `~/kist-data-collector-sessions`). Keep `storage.output_dir` under
`sessions/` or recordings land in the container's filesystem instead.

```bash
# 0. once: transmitter on (its own container/machine)
docker start kist-ext-sensor-io 2>/dev/null || true
docker exec -d kist-ext-sensor-io ./build/test_realsense_transmitter

# enter the collector container (build the image once with ./docker/build.sh)
./docker/run.sh
```

Inside the container:

```bash
# 1. record — Ctrl-C to stop (or prefix: timeout --signal=INT 12 ...)
#    (recording only some cameras? comment the rest out of realsense_cameras)
./build/kist_data_collector config/config.yaml
S=$(ls -dt sessions/*/ | head -1)/head

# 2. structural check
python3 scripts/verify_session.py "$(dirname "$S")"

# 3. export the whole session to mp4 — one color (remux) + one depth
#    (decode+colorize) thread per camera, all cameras concurrently
./build/export_session_mp4 "$(dirname "$S")"
ffplay "$S/color.mp4"
ffplay "$S/depth.mp4"
```

## Exporting

Two session-level converters, both detecting the camera dirs and running
one color + one depth worker thread per camera (3 cameras → 6 threads;
the `include/export/` classes expose start/stop/wait for embedding):

```bash
./build/export_session_mp4    <session_dir>   # for eyes: playable videos
./build/export_session_images <session_dir>   # for training: per-frame images
```

- `export_session_mp4` → `<camera>/color.mp4` (libavformat remux, no
  re-encode, per-frame `recv_ns` timestamps on a 90 kHz track, cut at the
  first keyframe) + `<camera>/depth.mp4` (RVL decode → JET colorize
  0.3–4 m → encode at measured fps; a *preview*, not data).
- `export_session_images` → `<camera>/color_jpg/<seq>.jpg` (q95 — the
  source is H.264-lossy already, so PNG would only preserve codec artifacts
  at 4-5× the bytes) + `<camera>/depth_png/<seq>.png` (16-bit lossless,
  pixel × `depth_scale` = meters — depth must stay PNG: JPEG is 8-bit and
  lossy). Files are named by wire `seq`, shared between a camera's color
  and depth, so cross-modal pairing is a filename match; `seq → recv_ns`
  (for lowstate/hand alignment) comes from the idx CSVs.

Fidelity note: frames before the first keyframe (≤1 s) are undecodable by
nature and skipped in both export forms — they remain in `color.h264`.
