cmake_minimum_required(VERSION 3.18)
project(kist_data_collector CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(yaml-cpp REQUIRED)

# ─── thirdparty: kist-ext-sensor-io ───────────────────────────────────────────
# Cloned pinned by the Docker image build (manual builds: see README "Build").
# Brings unitree_sdk2, the idlc codegen (kist_idl), the DDS subscribers
# (realsense_rx) and the config parser (realsense_config). The recorder only
# needs the Rx wire path — no librealsense/x264; their guarded targets simply
# don't build where those deps are absent.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/kist-ext-sensor-io/CMakeLists.txt)
    message(FATAL_ERROR
        "thirdparty/kist-ext-sensor-io missing — build via docker/build.sh, or "
        "clone it (pinned) as shown in README.md's Build section.")
endif()
add_subdirectory(thirdparty/kist-ext-sensor-io)

if(NOT TARGET realsense_rx)
    message(FATAL_ERROR
        "realsense_rx unavailable — idlc not found (/opt/cyclonedds/bin). "
        "Build inside the docker image (docker/build.sh && docker/run.sh) or "
        "install the CycloneDDS 0.10.2 idlc toolchain (see thirdparty snapshot README).")
endif()

# ─── recorder library ─────────────────────────────────────────────────────────
# common/: stream-agnostic pieces (RecordQueue hand-off, blob/row writers,
# session dir + meta) that every domain recorder builds on.
# realsense_recorder/: the RealSense assembly — subscriber on_frame hooks ->
# bounded queues -> writer threads appending the compressed payloads + CSV
# indices (see include/common/record_queue.hpp for the losslessness contract).
add_library(realsense_recorder STATIC
    src/realsense_recorder/realsense_recorder.cpp
    src/common/session.cpp
)
target_include_directories(realsense_recorder PUBLIC include)
target_link_libraries(realsense_recorder PUBLIC realsense_rx)

# Robot-state recorders: rt/lowstate (unitree_hg LowState_, ~1 kHz) and the
# Dex3 hands (rt/dex3/<side>/state — fingers are NOT in lowstate) -> one CSV
# row per message via the row writer. Each recorder instance owns its queue
# + writer thread, so hands and lowstate never share a storage path. Needs
# only the SDK (typed subscribers + the bundled hg IDL types) — no camera deps.
add_library(unitree_recorder STATIC
    src/unitree_recorder/lowstate_recorder.cpp
    src/unitree_recorder/dex3_recorder.cpp
)
target_include_directories(unitree_recorder PUBLIC include)
target_link_libraries(unitree_recorder PUBLIC unitree_sdk2)

# UWB recorder: ext's UwbSubscriber (SDK path) -> one CSV row per fix.
add_library(uwb_recorder STATIC src/uwb_recorder/uwb_recorder.cpp)
target_include_directories(uwb_recorder PUBLIC include)
target_link_libraries(uwb_recorder PUBLIC uwb_io)

# ─── whole-app assembly ───────────────────────────────────────────────────────
# DataCollector — every enabled recorder behind one start/stop, into one
# session dir (settings from yaml, 1 Hz report, drain + meta summary on stop).
add_library(data_collector STATIC src/system/data_collector.cpp)
target_include_directories(data_collector PUBLIC include)
target_link_libraries(data_collector
    PUBLIC realsense_recorder unitree_recorder uwb_recorder yaml-cpp)

# ─── runners ──────────────────────────────────────────────────────────────────
# Main collector: config -> DataCollector, report loop, stop on signal.
add_executable(kist_data_collector src/main.cpp)
target_link_libraries(kist_data_collector PRIVATE data_collector)

# ─── offline export (optional) ────────────────────────────────────────────────
# export/: start/stop worker classes, one thread per stream —
#   ExportColorToMp4    color.h264 -> color.mp4 (libavformat remux, VFR)
#   ExportDepthToMp4    depth.rvl  -> depth.mp4 (RVL -> colorize -> encode)
#   ExportColorToImages color.h264 -> color_jpg/<seq>.jpg (training form)
#   ExportDepthToImages depth.rvl  -> depth_png/<seq>.png (16-bit, exact)
# The export_session_* runners start one worker pair per camera dir, so a
# whole session converts concurrently (3 cameras -> 6 threads). Guarded on
# libav + OpenCV + the vendored RVL decoder (all present in this image).
find_package(OpenCV QUIET)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(AVFMT QUIET libavformat libavcodec libavutil libswscale)
endif()
if(AVFMT_FOUND AND TARGET realsense_receiver AND OpenCV_FOUND)
    add_library(session_export STATIC
        src/export/export_color_to_mp4.cpp
        src/export/export_depth_to_mp4.cpp
        src/export/export_color_to_images.cpp
        src/export/export_depth_to_images.cpp
    )
    target_include_directories(session_export PUBLIC include
        PRIVATE ${AVFMT_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(session_export
        PUBLIC realsense_receiver ${AVFMT_LIBRARIES} ${OpenCV_LIBS})
    target_link_directories(session_export PUBLIC ${AVFMT_LIBRARY_DIRS})

    add_executable(export_session_mp4 tools/export_session_mp4.cpp)
    target_link_libraries(export_session_mp4 PRIVATE session_export)

    add_executable(export_session_images tools/export_session_images.cpp)
    target_link_libraries(export_session_images PRIVATE session_export)
else()
    message(STATUS "libav/OpenCV/realsense_receiver missing — skipping session export")
endif()
