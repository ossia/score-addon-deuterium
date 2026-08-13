project(score_addon_deuterium LANGUAGES CXX)

if(NOT TARGET score_plugin_engine)
  return()
endif()

score_common_setup()

# libgig-modified sources (compiled directly into the plugin)
set(LIBGIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libgig-modified")

file(STRINGS "${LIBGIG_DIR}/configure.ac" LIBGIG_VERSION_STRINGS REGEX "^m4_define\\(libgig_release.*\\)$")
string(REGEX MATCH "libgig_release_major, ([^)]*)\\)" _TEMP ${LIBGIG_VERSION_STRINGS})
set(LIBGIG_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "libgig_release_minor, ([^)]*)\\)" _TEMP ${LIBGIG_VERSION_STRINGS})
set(LIBGIG_VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "libgig_release_build, ([^)]*)\\)" _TEMP ${LIBGIG_VERSION_STRINGS})
set(LIBGIG_VERSION_BUILD ${CMAKE_MATCH_1})

set(LIBGIG_SRCS
  "${LIBGIG_DIR}/src/DLS.cpp"
  "${LIBGIG_DIR}/src/gig.cpp"
  "${LIBGIG_DIR}/src/helper.cpp"
  "${LIBGIG_DIR}/src/Korg.cpp"
  "${LIBGIG_DIR}/src/RIFF.cpp"
  "${LIBGIG_DIR}/src/Serialization.cpp"
  "${LIBGIG_DIR}/src/SF.cpp"
  "${LIBGIG_DIR}/src/typeinfo.cpp"
)

# Files & main target
set(HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Executor/Component.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Drumkit.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessFactory.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessMetadata.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModel.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/Executor/Component.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/GigLoader.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/ProcessFactory.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/ProcessMetadata.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/ProcessModel.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/Library.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_deuterium.hpp"
)
set(SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Drumkit.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModelSerialization.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Executor/Component.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/GigLoader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/ProcessModel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/ProcessModelSerialization.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/GigSampler/Executor/Component.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_deuterium.cpp"

  ${LIBGIG_SRCS}
)

add_library(${PROJECT_NAME} ${SRCS} ${HDRS})

# TODO make this automatic in some way, and extend to other components
score_generate_command_list_file(${PROJECT_NAME} "${HDRS}")

# libgig compile definitions and include path
target_include_directories(${PROJECT_NAME} PRIVATE "${LIBGIG_DIR}/src")
target_compile_definitions(${PROJECT_NAME} PRIVATE
  PACKAGE="libgig"
  VERSION="${LIBGIG_VERSION_MAJOR}.${LIBGIG_VERSION_MINOR}.${LIBGIG_VERSION_BUILD}"
)
if(WIN32)
  target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX WIN32)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_definitions(${PROJECT_NAME} PRIVATE WINDOWS_CLANG_SCXT)
  else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE WINDOWS_MSVC_SCXT)
  endif()
endif()

# Disable unity build for libgig sources (they don't support it)
foreach(_src ${LIBGIG_SRCS})
  set_source_files_properties(${_src} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
endforeach()

target_link_libraries(
  ${PROJECT_NAME}
  PUBLIC
    score_lib_base score_plugin_engine score_plugin_media

  PRIVATE
    ${QT_PREFIX}::Xml

    $<BUILD_INTERFACE:dspfilters>
    $<BUILD_INTERFACE:gamma>
)

setup_score_plugin(${PROJECT_NAME})
