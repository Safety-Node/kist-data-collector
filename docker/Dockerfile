# kist-data-collector — storage-side recorder for kist-ext-sensor-io streams.
# The record path persists the compressed payloads verbatim (never decodes), so
# it drops ext's Tx-side capture/encode deps (librealsense, x264) — just the DDS
# + idlc toolchain. OpenCV/libav/ffmpeg are bundled only so the whole
# record -> verify -> export -> play loop runs in this one container.
# Base is plain ubuntu:24.04 (no CUDA), same as kist-ext-sensor-io. Recipe
# mirrors ext otherwise: unitree_sdk2 cloned (pinned) inside, idlc toolchain in
# /opt, repo copied in and built at image-build time — `docker run` gives
# ready binaries.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# ─── build tools + light deps ─────────────────────────────────────────────────
# nano is baked in for editing the config without leaving the container (a
# config change alone doesn't need an image rebuild).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    ca-certificates \
    libyaml-cpp-dev \
    nano \
    && rm -rf /var/lib/apt/lists/*

# ─── idlc toolchain (custom DDS IDL types) ────────────────────────────────────
# CycloneDDS 0.10.2 pinned to match unitree_sdk2's bundled libddscxx (same
# rationale + layout as the kist-ext-sensor-io image: /opt so the build tools
# never shadow the SDK's vendored runtime libs).
ARG CYCLONEDDS_VERSION=0.10.2
RUN git clone --depth 1 --branch "${CYCLONEDDS_VERSION}" \
        https://github.com/eclipse-cyclonedds/cyclonedds.git /tmp/cyclonedds && \
    cmake -S /tmp/cyclonedds -B /tmp/cyclonedds/build \
        -DCMAKE_INSTALL_PREFIX=/opt/cyclonedds \
        -DBUILD_IDLC=ON -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/cyclonedds/build --target install -j "$(nproc)" && \
    git clone --depth 1 --branch "${CYCLONEDDS_VERSION}" \
        https://github.com/eclipse-cyclonedds/cyclonedds-cxx.git /tmp/cyclonedds-cxx && \
    cmake -S /tmp/cyclonedds-cxx -B /tmp/cyclonedds-cxx/build \
        -DCMAKE_INSTALL_PREFIX=/opt/cyclonedds \
        -DCMAKE_PREFIX_PATH=/opt/cyclonedds -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/cyclonedds-cxx/build --target install -j "$(nproc)" && \
    rm -rf /tmp/cyclonedds /tmp/cyclonedds-cxx
ENV PATH=/opt/cyclonedds/bin:${PATH}
ENV CYCLONEDDS_HOME=/opt/cyclonedds

# ─── export + playback deps ───────────────────────────────────────────────────
# So the whole record → verify → export → play loop runs inside this one
# container: OpenCV + libav build export_depth_png (RVL -> PNG); ffmpeg
# brings ffplay (playback via the X11 socket run.sh mounts) and the mp4
# remux for export_color_mp4.py. Recording itself uses none of these.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libopencv-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/kist-data-collector

# ─── thirdparty: kist-ext-sensor-io (cloned + pinned) ─────────────────────────
# Cloned at image build instead of vendored in this repo. Pinned to the
# commit the collector was validated against — bump the ARG (or
# --build-arg) to move to a newer upstream.
# Placed before COPY so source edits don't re-clone; .dockerignore keeps any
# local working copy out of the build context so this pinned clone wins.
ARG EXT_SENSOR_IO_REPO=https://github.com/Safety-Node/kist-ext-sensor-io.git
# 94848e6: depth frames now carry camera intrinsics (fx/fy/cx/cy) — the
# CompressedDepthFrame IDL grew four fields, so the collector must build
# against this ext to stay wire-compatible with the transmitter.
ARG EXT_SENSOR_IO_COMMIT=94848e6989d1b10a36aa2651e9c91375e2875972
RUN git clone "${EXT_SENSOR_IO_REPO}" thirdparty/kist-ext-sensor-io && \
    git -C thirdparty/kist-ext-sensor-io checkout "${EXT_SENSOR_IO_COMMIT}"

# ─── thirdparty: unitree_sdk2 (cloned + pinned) ───────────────────────────────
# Goes inside the ext-sensor-io tree, where its CMake expects it.
ARG UNITREE_SDK2_COMMIT=21d0a3b2c46ee48c8fdf2783becb6be3beb0a59b
RUN git clone https://github.com/unitreerobotics/unitree_sdk2.git \
        thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2 && \
    git -C thirdparty/kist-ext-sensor-io/thirdparty/unitree_sdk2 \
        checkout "${UNITREE_SDK2_COMMIT}"

# ─── source + build ───────────────────────────────────────────────────────────
COPY . .
RUN cmake -B build && cmake --build build -j"$(nproc)"

# NOTE: no CYCLONEDDS_URI here on purpose. The unitree SDK ignores that env
# whenever a network interface is passed to ChannelFactory::Init, so the
# collector builds the URI itself at startup (interface + receive tuning —
# see src/main.cpp) and hands the SDK an empty interface. The host must
# still allow the buffer request: net.core.rmem_max (README "Deployment").

CMD ["/bin/bash"]
