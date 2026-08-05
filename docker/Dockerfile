# kist-data-collector — storage-side recorder for kist-ext-sensor-io streams.
# Rx-wire-path deps only: no librealsense/x264/libav/OpenCV (the collector
# persists the compressed payloads verbatim, it never decodes), so a slim
# ubuntu base instead of ext-sensor-io's tensorrt one. Same recipe otherwise:
# unitree_sdk2 cloned (pinned) inside, idlc toolchain in /opt, repo copied in
# and built at image-build time — `docker run` gives ready binaries.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# ─── build tools + deps ───────────────────────────────────────────────────────
# python3 is for scripts/verify_session.py (session integrity checks in-image).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    ca-certificates \
    libyaml-cpp-dev \
    python3 \
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

# In-container quality of life: editing the baked config without leaving
# the container (a config change alone doesn't need an image rebuild).
RUN apt-get update && apt-get install -y --no-install-recommends \
    nano \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace/kist-data-collector

# ─── thirdparty: kist-ext-sensor-io (cloned + pinned) ─────────────────────────
# Cloned at image build instead of vendored in this repo. Pinned to the
# commit the collector was validated against — bump the ARG (or
# --build-arg) to move to a newer upstream.
# Placed before COPY so source edits don't re-clone; .dockerignore keeps any
# local working copy out of the build context so this pinned clone wins.
ARG EXT_SENSOR_IO_REPO=https://github.com/Safety-Node/kist-ext-sensor-io.git
# a8de3ae: runner DDS routing + reader queue 30 + MTU-capped datagrams
ARG EXT_SENSOR_IO_COMMIT=a8de3ae293ab55354fc28b054146f0d040ca7e55
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
