project(score_addon_videoio LANGUAGES CXX)

score_common_setup()

if(NOT TARGET score_plugin_gfx)
  message(STATUS "score-addon-videoio: requires score_plugin_gfx, skipping")
  return()
endif()

find_package(${QT_VERSION} REQUIRED COMPONENTS Core Gui Widgets)

# ---------------------------------------------------------------------------
# Vendor SDK detection. Each vendor is independently optional and gated by a
# SCORE_HAS_<VENDOR> compile definition; the unified Direct Video I/O device
# hardcodes the compiled-in set (no registry).
# ---------------------------------------------------------------------------
set(VIDEOIO_SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_videoio.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoIOSettings.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoOutput.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoInput.cpp"
)
set(VIDEOIO_HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_videoio.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoIOSettings.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoOutput.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/VideoInput.hpp"
)
set(VIDEOIO_GEN_SRCS "")
set(VIDEOIO_DEFS "")
set(VIDEOIO_INCS "")

# --- AJA -------------------------------------------------------------------
# Vendored libajantv2 SDK (git submodule, built as a static lib). Merged in
# from the now-deprecated score-addon-aja; the device/GPU code is relocated
# verbatim. Independently optional: gated on the submodule being checked out.
set(VIDEOIO_AJA_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libajantv2"
    CACHE PATH "AJA SDK root directory")
if(EXISTS "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes/ntv2card.h")
  # Build the vendored AJA SDK as a static lib named `ajantv2`. Guarded so it
  # coexists with a still-present (deprecated) score-addon-aja in the same tree.
  if(NOT TARGET ajantv2)
    set(AJANTV2_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_DEMOS       ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_TOOLS       ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_PLUGIN_LOAD ON  CACHE BOOL "" FORCE)
    set(AJANTV2_DISABLE_TESTS       ON  CACHE BOOL "" FORCE)
    add_subdirectory("${VIDEOIO_AJA_SDK_ROOT}"
                     "${CMAKE_CURRENT_BINARY_DIR}/libajantv2"
                     EXCLUDE_FROM_ALL)
  endif()

  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJACaptureSession.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJACpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInputNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutput.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutputNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaFormatMap.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaDmaLock.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropCpu.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropD3D11Dvp.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropD3D11Tier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropGLDvp.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropGLTier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/CaptureInteropVulkanTier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropD3D11Dvp.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropD3D11Tier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropD3D12Tier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropGLDvp.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropGLTier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/RdmaInteropVulkanTier3.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/Tier3Common.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJACaptureSession.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAInputNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AJAOutputNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AJA/AjaOutputBackend.cpp")
  list(APPEND VIDEOIO_INCS
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes"
    "${VIDEOIO_AJA_SDK_ROOT}")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_AJA=1 SCORE_HAS_AJA_CUDA_BRIDGE=1)
  set(VIDEOIO_HAS_AJA TRUE)
  message(STATUS "score-addon-videoio: AJA enabled (SDK: ${VIDEOIO_AJA_SDK_ROOT})")
else()
  message(STATUS "score-addon-videoio: AJA disabled (submodule 3rdparty/libajantv2 not checked out)")
endif()

# --- Blackmagic DeckLink -----------------------------------------------------
# Windows + clang64: the SDK ships only .idl (MS MIDL input); we generate the
# COM C++ header + IID source with MinGW's widl at build time.
set(DECKLINK_SDK_ROOT "c:/dev/blackmagic-sdk" CACHE PATH "Blackmagic DeckLink SDK root")
find_program(WIDL_EXECUTABLE widl)
if(WIN32 AND WIDL_EXECUTABLE AND EXISTS "${DECKLINK_SDK_ROOT}/Win/include/DeckLinkAPI.idl")
  set(DL_IDL_DIR "${DECKLINK_SDK_ROOT}/Win/include")
  set(DL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/decklink_gen")
  file(GLOB DL_IDLS "${DL_IDL_DIR}/*.idl")
  add_custom_command(
    OUTPUT "${DL_GEN_DIR}/DeckLinkAPI_h.h" "${DL_GEN_DIR}/DeckLinkAPI_i.c"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_decklink_headers.sh"
            "${DL_IDL_DIR}" "${DL_GEN_DIR}" "${WIDL_EXECUTABLE}"
    DEPENDS ${DL_IDLS}
    COMMENT "score-addon-videoio: generating DeckLink COM headers via widl"
    VERBATIM)

  list(APPEND VIDEOIO_HDRS
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLink.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkComPtr.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkFormats.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCpuCapture.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkModeMap.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkCaptureNode.cpp")
  list(APPEND VIDEOIO_GEN_SRCS "${DL_GEN_DIR}/DeckLinkAPI_i.c")
  list(APPEND VIDEOIO_INCS "${DL_GEN_DIR}")
  list(APPEND VIDEOIO_DEFS SCORE_HAS_DECKLINK=1)
  message(STATUS "score-addon-videoio: DeckLink enabled (widl: ${WIDL_EXECUTABLE})")
else()
  message(STATUS "score-addon-videoio: DeckLink disabled (need WIN32 + widl + SDK)")
endif()

# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME}
  ${VIDEOIO_SRCS} ${VIDEOIO_HDRS} ${VIDEOIO_GEN_SRCS})

# The generated DeckLinkAPI_i.c is C with COM GUIDs; compile as-is (no PCH).
if(VIDEOIO_GEN_SRCS)
  set_source_files_properties(${VIDEOIO_GEN_SRCS} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

target_include_directories(${PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${VIDEOIO_INCS})

target_link_libraries(${PROJECT_NAME}
  PUBLIC
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets
    score_lib_base score_lib_device
    score_plugin_deviceexplorer score_lib_process
    score_lib_state score_lib_inspector
    score_plugin_gfx
    score_plugin_media)

if(VIDEOIO_DEFS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ${VIDEOIO_DEFS})
endif()

# --- AJA link + GPU-direct bridges ------------------------------------------
if(VIDEOIO_HAS_AJA)
  target_link_libraries(${PROJECT_NAME} PRIVATE ajantv2)

  # Tier-3 GPU<->SDI on Windows uses NVIDIA DVP; the bridge lives in
  # score_plugin_gfx (score_nv_dvp_bridge target) and is runtime-loaded — no
  # link-time CUDAToolkit dependency. Source-side gate name preserved.
  if(TARGET score_nv_dvp_bridge)
    target_link_libraries(${PROJECT_NAME} PRIVATE score_nv_dvp_bridge)
    target_compile_definitions(${PROJECT_NAME} PRIVATE SCORE_HAS_AJA_DVP_BRIDGE=1)
    message(STATUS "score-addon-videoio: linking score_nv_dvp_bridge")

    if(WIN32 AND SCORE_NV_DVP_DLL AND EXISTS "${SCORE_NV_DVP_DLL}")
      add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SCORE_NV_DVP_DLL}"
          "$<TARGET_FILE_DIR:${PROJECT_NAME}>/dvp.dll"
        COMMENT "Copying dvp.dll alongside ${PROJECT_NAME}")
    endif()
  endif()
endif()

setup_score_plugin(${PROJECT_NAME})

# --- AJA card-to-card round-trip test harness (opt-in) ----------------------
# Relocated from score-addon-aja; off by default since it needs the full plugin
# set + real hardware. Drives the real output path and CPU-staging capture.
option(SCORE_VIDEOIO_BUILD_TESTS "Build the Direct Video I/O (AJA) test harness" OFF)
if(SCORE_VIDEOIO_BUILD_TESTS AND VIDEOIO_HAS_AJA)
  add_executable(AJARoundtrip tests/AJARoundtrip.cpp)
  target_include_directories(AJARoundtrip PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes"
    "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(AJARoundtrip PRIVATE
    ${PROJECT_NAME} ajantv2
    ${QT_PREFIX}::Core ${QT_PREFIX}::Gui ${QT_PREFIX}::Widgets)

  if(SCORE_STATIC_PLUGINS)
    target_link_libraries(AJARoundtrip PRIVATE ${SCORE_PLUGINS_LIST})
  endif()

  target_compile_definitions(AJARoundtrip PRIVATE SCORE_HAS_AJA=1)
  target_compile_definitions(AJARoundtrip PRIVATE SCORE_HAS_AJA_CUDA_BRIDGE=1)

  # SDK-only (no score/Qt) reproducer for the ARGB->SDI black-output issue.
  add_executable(aja_rgb_repro tests/aja_rgb_repro.cpp)
  target_include_directories(aja_rgb_repro PRIVATE
    "${VIDEOIO_AJA_SDK_ROOT}/ajantv2/includes" "${VIDEOIO_AJA_SDK_ROOT}")
  target_link_libraries(aja_rgb_repro PRIVATE ajantv2)
endif()
