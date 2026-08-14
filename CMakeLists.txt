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

# ─── common ───────────────────────────────────────────────────────────────────
# Stream-agnostic pieces every recorder builds on: RecordQueue hand-off and
# the blob/row writers are header-only; session dir + meta.yaml compile here.
add_library(recorder_common STATIC src/common/session.cpp)
target_include_directories(recorder_common PUBLIC include)

# ─── per-stream recorder assemblies (system/, embeddable libraries) ───────────
# One assembly per stream behind start/stop — subscriber on_frame/on_msg
# hooks -> bounded queue(s) -> writer thread(s) (see
# include/common/record_queue.hpp for the losslessness contract). The test_*
# runners drive one each; DataCollector below composes them.

# RealsenseRecorder: compressed color/depth payloads verbatim + CSV indices.
add_library(realsense_recorder STATIC src/system/realsense_recorder.cpp)
target_include_directories(realsense_recorder PUBLIC include)
target_link_libraries(realsense_recorder PUBLIC realsense_rx recorder_common)

# LowstateRecorder (rt/lowstate, ~1 kHz) + Dex3Recorder (rt/dex3/<side>/state
# — fingers are NOT in lowstate) -> one CSV row per message, plus their
# command (action) twins: LowcmdRecorder (rt/lowcmd | rt/arm_sdk, same
# LowCmd_ type) + Dex3CmdRecorder (rt/dex3/<side>/cmd). Each instance owns
# its queue + writer thread, so streams never share a storage path. Needs
# only the SDK — no camera deps.
add_library(unitree_recorder STATIC
    src/system/lowstate_recorder.cpp
    src/system/dex3_recorder.cpp
    src/system/lowcmd_recorder.cpp
    src/system/dex3_cmd_recorder.cpp
)
target_include_directories(unitree_recorder PUBLIC include)
target_link_libraries(unitree_recorder PUBLIC unitree_sdk2 recorder_common)

# UwbRecorder: ext's UwbSubscriber (SDK path) -> one CSV row per fix.
add_library(uwb_recorder STATIC src/system/uwb_recorder.cpp)
target_include_directories(uwb_recorder PUBLIC include)
target_link_libraries(uwb_recorder PUBLIC uwb_io recorder_common)

# MotionTokenRecorder: gearsonic's decoder-input token (rt/kist/motion_token)
# -> one CSV row per CONTROL tick. Wire type from idl/kist_msgs.idl — a COPY
# of the source of truth in kist-gearsonic-inference (its idl/kist_msgs.idl);
# keep the two in sync when the contract changes. CYCLONE_IDLC comes from
# the thirdparty snapshot's find_program (guarded above via realsense_rx).
set(KIST_MSGS_GEN ${CMAKE_BINARY_DIR}/kist_msgs_gen)
file(MAKE_DIRECTORY ${KIST_MSGS_GEN})
add_custom_command(
    OUTPUT ${KIST_MSGS_GEN}/kist_msgs.hpp ${KIST_MSGS_GEN}/kist_msgs.cpp
    COMMAND ${CYCLONE_IDLC} -l cxx ${CMAKE_CURRENT_SOURCE_DIR}/idl/kist_msgs.idl
    WORKING_DIRECTORY ${KIST_MSGS_GEN}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/idl/kist_msgs.idl
    COMMENT "idlc: kist_msgs.idl -> C++ DDS types")
add_library(kist_msgs_idl STATIC ${KIST_MSGS_GEN}/kist_msgs.cpp)
target_include_directories(kist_msgs_idl PUBLIC ${KIST_MSGS_GEN})
target_link_libraries(kist_msgs_idl PUBLIC unitree_sdk2)

add_library(gearsonic_recorder STATIC src/system/motion_token_recorder.cpp)
target_include_directories(gearsonic_recorder PUBLIC include)
target_link_libraries(gearsonic_recorder PUBLIC kist_msgs_idl recorder_common)

# ─── whole-app assembly (system/) ─────────────────────────────────────────────
# DataCollector — every enabled recorder behind one start/stop, into one
# session dir (settings from yaml, 1 Hz report, drain + meta summary on stop).
add_library(data_collector STATIC src/system/data_collector.cpp)
target_include_directories(data_collector PUBLIC include)
target_link_libraries(data_collector
    PUBLIC realsense_recorder unitree_recorder uwb_recorder gearsonic_recorder
           yaml-cpp)

# ─── runners ──────────────────────────────────────────────────────────────────
# Main collector: config -> DataCollector, report loop, stop on signal.
add_executable(kist_data_collector src/main.cpp)
target_link_libraries(kist_data_collector PRIVATE data_collector)

# ─── tests ────────────────────────────────────────────────────────────────────
# One-stream isolation runners — each drives its recorder assembly directly
# (own session dir, report, drain). Also the embedding reference per stream.
add_executable(test_realsense_recorder test/test_realsense_recorder.cpp)
target_link_libraries(test_realsense_recorder PRIVATE realsense_recorder yaml-cpp)

add_executable(test_lowstate_recorder test/test_lowstate_recorder.cpp)
target_link_libraries(test_lowstate_recorder PRIVATE unitree_recorder yaml-cpp)

add_executable(test_dex3_recorder test/test_dex3_recorder.cpp)
target_link_libraries(test_dex3_recorder PRIVATE unitree_recorder yaml-cpp)

add_executable(test_uwb_recorder test/test_uwb_recorder.cpp)
target_link_libraries(test_uwb_recorder PRIVATE uwb_recorder yaml-cpp)

add_executable(test_cmd_recorder test/test_cmd_recorder.cpp)
target_link_libraries(test_cmd_recorder PRIVATE unitree_recorder yaml-cpp)

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
