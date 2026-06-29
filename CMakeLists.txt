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
)
set(VIDEOIO_HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_videoio.hpp"
)
set(VIDEOIO_GEN_SRCS "")
set(VIDEOIO_DEFS "")
set(VIDEOIO_INCS "")

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
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.hpp")
  list(APPEND VIDEOIO_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkDevices.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkOutputBackend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/decklink/DeckLinkInputBackend.cpp")
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

setup_score_plugin(${PROJECT_NAME})
