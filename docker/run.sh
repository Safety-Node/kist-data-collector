#!/bin/bash
# Launch (or re-attach to) a persistent named container — same pattern as
# kist-ext-sensor-io's run.sh. The whole loop (record, verify, export,
# ffplay playback) runs inside this one container:
#
#   --network host       DDS discovery/multicast toward the sensor machine
#   -v ...sessions       recordings persist on the host, outside the container
#                        (every config's storage.output_dir points here)
#   DISPLAY + X11 socket ffplay windows show on the host display
#
# SESSIONS_DIR overrides where recordings land on the host.
#
# Iterative dev: add  -v "$(pwd)":/workspace/kist-data-collector  to shadow
# the baked source with your working copy (then rebuild build/ inside; the
# vendored unitree_sdk2 must then exist in your working copy too).

set -e

CONTAINER=kist-data-collector
SESSIONS_DIR="${SESSIONS_DIR:-$HOME/kist-data-collector-sessions}"

if [ "$(docker ps -q -f name=^${CONTAINER}$)" ]; then
    docker exec -it "${CONTAINER}" /bin/bash
elif [ "$(docker ps -aq -f name=^${CONTAINER}$)" ]; then
    docker start -ai "${CONTAINER}"
else
    mkdir -p "${SESSIONS_DIR}"
    xhost +local:root >/dev/null 2>&1 || true
    docker run -it \
        --name "${CONTAINER}" \
        --network host \
        -e DISPLAY="${DISPLAY}" \
        -v /tmp/.X11-unix:/tmp/.X11-unix \
        -v "${SESSIONS_DIR}":/workspace/kist-data-collector/sessions \
        -w /workspace/kist-data-collector \
        kist-data-collector
fi
