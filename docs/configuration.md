# Configuration

Two files under `config/`: `config.yaml` (streams + storage) and
`cyclonedds.xml` (DDS transport). Edit them per deployment — on Docker the
image bakes both in, so either rebuild after editing on the host, or edit
inside the container (`nano` is installed) for a one-off.

## `config.yaml`

### `unitree`

| Key | Default | Meaning |
|---|---|---|
| `domain_id` | `0` | DDS domain — must match the transmitter |
| `dds_config` | `config/cyclonedds.xml` | path to the DDS transport XML below. Routed via `CYCLONEDDS_URI` at startup (a pre-set env wins); a missing file is a fatal, loud error |

### `storage`

| Key | Default | Meaning |
|---|---|---|
| `output_dir` | `sessions` | session dirs are created as `<output_dir>/<YYYYMMDD_HHMMSS>/`. Keep it under `sessions/` — that's the path `docker/run.sh` mounts to the host; anything else stays inside the container |

### `realsense_cameras`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | the whole camera path on/off |
| `queue_capacity` | `1024` | in-flight frame bound per stream (30 fps → capacity/30 s of disk-stall headroom; memory ≈ capacity × compressed frame size) |
| `reliable` | `true` | camera readers request RELIABLE QoS — isolated wire losses are recovered by retransmission. `false` = plain best-effort, the safety valve if a transmitter ever stops offering RELIABLE |
| `cameras[].name` | — | selects the `rt/kist/camera/<name>/...` topics; must match the transmitter's camera names |
| `cameras[].enabled` | `true` | per-camera switch |

### `lowstate`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | record the G1's `rt/lowstate` into `lowstate.csv` |
| `queue_capacity` | `8192` | the stream runs at ~1 kHz — 8192 ≈ 8 s of headroom (~25 MB) |

### `dex3`

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | record both Dex3 hands (`rt/dex3/{left,right}/state`) into `hand_{left,right}.csv` |
| `queue_capacity` | `4096` | each hand runs at ~830 Hz — one recorder (own queue + writer thread) per hand |

## `cyclonedds.xml`

The network interface and every transport knob live **here, not in
config.yaml**: the unitree SDK builds its own DDS config whenever it is
handed a non-empty interface and then silently ignores `CYCLONEDDS_URI` —
so the collector always passes the SDK an empty interface and routes this
file instead.

| Element | Value | Meaning |
|---|---|---|
| `General/Interfaces/NetworkInterface name` | `eno2` | the NIC toward the robot LAN on this machine (`lo` for same-machine testing) — **the** place to change the NIC |
| `Internal/SocketReceiveBufferSize min` | `16MB` | kernel UDP receive buffer request. The host must allow it: `net.core.rmem_max` (see the README's Deployment tuning) — otherwise the request is silently clamped |
| `Internal/DefragUnreliableMaxSamples` | `64` | concurrent best-effort defrag slots (Cyclone default 4) — a ~190 KB depth frame is ~130 fragments and camera bursts overlap |
| `Internal/DeliveryQueueMaxSamples` | `1024` | the delivery queue all readers share (Cyclone default 256) — high-rate row streams + camera bursts overflow it otherwise |
