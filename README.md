# kist-data-collector

Storage-side recorder for the KIST G1 — records the
[kist-ext-sensor-io](https://github.com/Safety-Node/kist-ext-sensor-io)
cameras (H.264 color + RVL depth), `rt/lowstate` and the Dex3 hands into one
**lossless** session. Exporters turn a session into videos or training images.

## Architecture

[![Architecture](docs/kist-data-collector.svg)](docs/kist-data-collector.svg)

## Dependencies

| Component | Version | Role |
|---|---|---|
| `kist-ext-sensor-io` | `a8de3ae` | camera wire contract (`kist_msgs` IDL, frame structs, topic names) + RVL depth decoder |
| `unitree_sdk2` | `21d0a3b` | DDS client (lowstate / Dex3 subscribers), `hg` IDL types, bundled ddsc/ddscxx runtime |
| CycloneDDS + CycloneDDS-CXX | 0.10.2 | `idlc`/`idlcxx` codegen for the camera DDS types |
| `yaml-cpp` | distro | config parsing |
| FFmpeg / libav | distro | mp4 remux + H.264 decode (exporters), `ffplay` playback |
| OpenCV | distro | depth colorize/encode, jpg/png writing (exporters) |

All of it is baked into the Docker image — the pinned repos are cloned at
image build (see Build).

## Installation

#### 1. Clone Repository

```bash
git clone https://github.com/Safety-Node/kist-data-collector.git
cd kist-data-collector
```

All following steps run from the repository root.

#### Quick Start with Docker

The image bakes in everything below (pinned thirdparty clones, toolchain,
export deps, and the build):

```bash
./docker/build.sh      # builds the image (docker build -t kist-data-collector)
./docker/run.sh        # shell in the container; prebuilt binaries under build/
                       # recordings land in $SESSIONS_DIR (default ~/kist-data-collector-sessions)
```

`run.sh` wires `--network host` (DDS), the X11 socket (`ffplay` playback),
and the `sessions/` mount. The numbered steps below are the manual
(non-Docker) alternative.

#### 2. Install apt packages

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake git pkg-config \
    libyaml-cpp-dev \
    libopencv-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev ffmpeg
```

(The last line is for the exporters/playback only — recording itself needs
none of it.)

#### 3. Install CycloneDDS (idlc toolchain)

CycloneDDS + CycloneDDS-CXX 0.10.2 into `/opt/cyclonedds`, pinned to match
the SDK's bundled `libddscxx` — same recipe as kist-ext-sensor-io's README
(step 4 there), then `export PATH=/opt/cyclonedds/bin:$PATH`.

#### 4. Clone the pinned thirdparty repos

```bash
git clone https://github.com/Safety-Node/kist-ext-sensor-io.git thirdparty/kist-ext-sensor-io
git -C thirdparty/kist-ext-sensor-io checkout a8de3ae293ab55354fc28b054146f0d040ca7e55

git clone https://github.com/unitreerobotics/unitree_sdk2.git \
    thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2
git -C thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2 \
    checkout 21d0a3b2c46ee48c8fdf2783becb6be3beb0a59b
```

Both clones are gitignored; the upstream is used unmodified. To move to a
newer upstream, bump `EXT_SENSOR_IO_COMMIT` (Dockerfile ARG) or the SHA
above.

## Build

With Docker, the image is already built — this is the manual path:

```bash
cmake -B build && cmake --build build
```

## Storage format

One directory per recording session:

```
sessions/<YYYYMMDD_HHMMSS>/
  meta.yaml                  # session times + per-stream counters
  lowstate.csv               # robot state, one row per message
  hand_left.csv hand_right.csv
  <camera>/
    color.h264  color.idx.csv    # compressed payloads verbatim,
    depth.rvl   depth.idx.csv    #   one CSV index per stream
```

Every file shares the `recv_ns` column (arrival clock, epoch ns) for
cross-stream alignment. Field-level schemas, format rationale and the
alternatives considered: [docs/storage-format.md](docs/storage-format.md).

## Usage

```bash
./build/kist_data_collector [config/config.yaml]
```

- Streams are switched in `config/config.yaml` — every section has
  `enabled` + `queue_capacity`; camera `name`s must match the transmitter's.
- Records until Ctrl-C into `sessions/<timestamp>/`, printing per-stream
  status once a second and writing the final counters to `meta.yaml`.

Healthy output: `rx` = `wr`, and `drop` / `werr` / `gap` all 0 —
`drop`/`werr` are recorder-side loss (queue / disk; 0 means every received
frame is on disk), `gap` counts frames that never arrived (the camera
readers recover isolated wire losses via RELIABLE retransmission).

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

The **sender** side is handled by kist-ext-sensor-io itself since commit
`d97b554`: its runners route `config/cyclonedds.xml` the same way (the XML
carries the NIC, `SocketSendBufferSize 16MiB`, and MTU-capped
`MaxMessageSize` — adopted after tcpdump showed it eliminates IP
fragmentation of camera datagrams entirely). On the sender host, raise
`net.core.wmem_max`/`rmem_max` (16 MiB is enough) the same `/etc/sysctl.d/`
way, and keep the Jetson in performance mode (`jetson_clocks` — a systemd
oneshot service survives reboots; clock state directly shows up as
encoder-skip losses).

Loss ledger from the robot-LAN measurements (2026-07/08), in the order the
causes were found and fixed:

| loss channel | scale | fix |
|---|---|---|
| receiver UDP rcvbuf overflow | ~0.9% of depth | host `rmem_max` + 16 MiB request |
| sender sndbuf / Jetson clocks | ~0.4% | `wmem_max` + 16 MiB + `jetson_clocks` |
| kernel IP reassembly (14 KB datagrams) | part of residual | `MaxMessageSize` MTU cap (sender) |
| residual transit losses | ~0.1%, isolated 1-4 frames | **RELIABLE readers** — retransmission recovers them |
| transmitter-side skips (head camera under load) | ~0.02%, the current floor | Jetson performance mode; inherent to the Tx latest-wins pipeline |

End state (10-min run, 2 cameras + lowstate + hands): ~0.02% camera frames
missing (all seq-recorded), every other stream zero, recorder side lossless
in every run ever measured (`dropped`/`write_errors` 0). The residual is
best handled at dataset-build time: gate episodes on their per-stream
wire_gap counters (meta.yaml).

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

# 2. export the whole session to mp4 — one color (remux) + one depth
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
