cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Satellite needs the frontend API (Tools menu, dock, scene/studio-mode events for tally)
# and Qt (the Satellite window). Both default ON here, unlike the upstream template.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Neither NDI nor OMT is linked at build time. Both runtimes are resolved through
# src/transport/library-loader at module load, so a missing runtime degrades to
# "protocol unavailable" in the Satellite window instead of a module that fails to load.
# On Unix that needs libdl for dlopen/dlsym.
if(NOT OS_WINDOWS)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# lib/ndi and lib/omt hold vendored interface headers for the two runtimes. They describe each
# ABI, but nothing is linked against them - both libraries are loaded at run time.
target_include_directories(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/lib/ndi" "${CMAKE_CURRENT_SOURCE_DIR}/lib/omt"
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/config/config.cpp
    src/config/config.hpp
    src/discovery/discovery-service.cpp
    src/discovery/discovery-service.hpp
    src/metrics/feed-registry.cpp
    src/metrics/feed-registry.hpp
    src/obs/satellite-filter.cpp
    src/obs/satellite-filter.hpp
    src/obs/satellite-output.cpp
    src/obs/satellite-output.hpp
    src/obs/satellite-source.cpp
    src/obs/satellite-source.hpp
    src/obs/sender-session.cpp
    src/obs/sender-session.hpp
    src/transport/library-loader.cpp
    src/transport/library-loader.hpp
    src/transport/ndi/ndi-backend.cpp
    src/transport/ndi/ndi-backend.hpp
    src/transport/omt/omt-api.cpp
    src/transport/omt/omt-api.hpp
    src/transport/omt/omt-backend.cpp
    src/transport/omt/omt-backend.hpp
    src/transport/rate-meter.hpp
    src/transport/transport.cpp
    src/transport/transport.hpp
    src/ui/satellite-dock.cpp
    src/ui/satellite-dock.hpp
    src/ui/sparkline.cpp
    src/ui/sparkline.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

option(ENABLE_TESTS "Build Satellite's test harnesses" OFF)

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
