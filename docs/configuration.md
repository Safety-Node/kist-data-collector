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
| `reliable` | `true` | RELIABLE reader QoS — lost frames are recovered by retransmission; `false` = plain best-effort |
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

The network interface and the DDS transport tuning live here — **not in
config.yaml**.

| Element | Value | Meaning |
|---|---|---|
| `General/Interfaces/NetworkInterface name` | `lo` | default is same-machine testing; for deployment set the robot-LAN NIC (e.g. `eno2`, `eth0`) here |
| `Internal/SocketReceiveBufferSize min` | `16MB` | UDP receive buffer request — requires `net.core.rmem_max` raised on the host (README's Deployment tuning) |
| `Internal/DefragUnreliableMaxSamples` | `64` | concurrent defragmentation slots |
| `Internal/DeliveryQueueMaxSamples` | `1024` | reader delivery queue depth |
