project(score_addon_deuterium_tests LANGUAGES CXX)
enable_testing()

if(SCORE_STATIC_PLUGINS)
  # In static-plugin builds, MinimalApplication's score_init_static_plugins()
  # references every plugin and addon class; these test executables link only
  # the libraries they use (and compile the addon sources directly), so they
  # cannot resolve those symbols there.
  return()
endif()

find_package(${QT_VERSION} REQUIRED COMPONENTS Core Test Xml)
set(CMAKE_AUTOMOC ON)

set(DEUTERIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The addon's symbols are not exported, so the tested sources are compiled
# directly into each executable instead of linking the plugin.
function(add_deuterium_tool TESTNAME TESTSRCS)
  add_executable(${TESTNAME} ${TESTSRCS} ${ARGN}
    "${DEUTERIUM_ROOT}/Deuterium/GigSampler/GigLoader.cpp"
    ${LIBGIG_SRCS}
  )
  setup_score_common_test_features(${TESTNAME})
  # The score PCH is compiled for the shared libraries (-fPIC); it cannot be
  # reused by a PIE test executable.
  set_target_properties(${TESTNAME} PROPERTIES DISABLE_PRECOMPILE_HEADERS TRUE)
  # Test sources call the serialization visitors directly; merging them with
  # the addon's serialization TU breaks explicit specialization ordering.
  set_target_properties(${TESTNAME} PROPERTIES UNITY_BUILD FALSE)
  target_include_directories(${TESTNAME} PRIVATE
    "${DEUTERIUM_ROOT}"
    "${LIBGIG_DIR}/src"
  )
  if(LIBGIG_COMPAT_DIR)
    target_include_directories(${TESTNAME} PRIVATE "${LIBGIG_COMPAT_DIR}/src")
  endif()
  target_compile_definitions(${TESTNAME} PRIVATE
    PACKAGE="libgig"
    VERSION="${LIBGIG_VERSION_MAJOR}.${LIBGIG_VERSION_MINOR}.${LIBGIG_VERSION_BUILD}"
  )
  target_link_libraries(${TESTNAME} PRIVATE
    score_lib_base score_plugin_engine score_plugin_media score_plugin_dataflow
    ${QT_PREFIX}::Test ${QT_PREFIX}::Xml
  )
  foreach(_src ${LIBGIG_SRCS})
    set_source_files_properties(${_src} PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
  endforeach()
endfunction()

function(add_deuterium_test TESTNAME TESTSRCS)
  add_deuterium_tool(${TESTNAME} ${TESTSRCS} ${ARGN})
  add_test(NAME ${TESTNAME}_target COMMAND ${TESTNAME})
endfunction()

add_deuterium_test(Deuterium_GigLoaderTest
  "${CMAKE_CURRENT_SOURCE_DIR}/GigLoaderTest.cpp"
)

add_deuterium_test(Deuterium_SamplerEngineTest
  "${CMAKE_CURRENT_SOURCE_DIR}/SamplerEngineTest.cpp"
)

add_deuterium_test(Deuterium_ProcessModelTest
  "${CMAKE_CURRENT_SOURCE_DIR}/ProcessModelTest.cpp"
  "${DEUTERIUM_ROOT}/Deuterium/GigSampler/ProcessModel.cpp"
  "${DEUTERIUM_ROOT}/Deuterium/GigSampler/ProcessModelSerialization.cpp"
)

# Manual tool, not a ctest test: scans user-installed sample packs (which are
# machine-specific and huge) and loads each file in an isolated subprocess
# through the same loader path score uses, to find crashing files.
add_deuterium_tool(Deuterium_PackTester
  "${CMAKE_CURRENT_SOURCE_DIR}/PackTester.cpp"
)
