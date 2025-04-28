project(score_plugin_deuterium LANGUAGES CXX)

if(NOT TARGET score_plugin_engine)
  return()
endif()

score_common_setup()

# Files & main target
set(HDRS
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Commands/EditScript.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Commands/ScriptMacro.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Commands/CommandFactory.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Executor/Component.hpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Drumkit.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessFactory.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessMetadata.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModel.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_deuterium.hpp"
)
set(SRCS
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Drumkit.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModel.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/ProcessModelSerialization.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Executor/Component.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/Deuterium/Commands/CommandFactory.cpp"

  "${CMAKE_CURRENT_SOURCE_DIR}/score_addon_deuterium.cpp"
)

add_library(${PROJECT_NAME} ${SRCS} ${HDRS})

# TODO make this automatic in some way, and extend to other components
score_generate_command_list_file(${PROJECT_NAME} "${HDRS}")

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
