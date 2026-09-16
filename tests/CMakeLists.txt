# Test harnesses for Satellite. Off by default, so neither the plugin build nor CI packaging
# is affected; enable with -DENABLE_TESTS=ON.
#
# See tests/README.md for why the fake runtime exists.

if(OS_WINDOWS)
  message(STATUS "Satellite tests are not built on Windows yet; skipping.")
  return()
endif()

# The fake runtime has to be named exactly what the backend searches for, because the test
# points NDI_RUNTIME_DIR_V6 at this directory and lets the real loader find it.
if(OS_MACOS)
  set(_fake_ndi_output "ndi")
  set(_fake_ndi_suffix ".dylib")
else()
  set(_fake_ndi_output "ndi")
  set(_fake_ndi_suffix ".so.6")
endif()

add_library(fake-ndi SHARED fake-ndi.cpp)
target_include_directories(fake-ndi PRIVATE "${CMAKE_SOURCE_DIR}/lib/ndi")
set_target_properties(
  fake-ndi
  PROPERTIES
    OUTPUT_NAME ${_fake_ndi_output}
    PREFIX "lib"
    SUFFIX ${_fake_ndi_suffix}
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime"
)

add_executable(
  ndi-backend-test
  ndi-backend-test.cpp
  "${CMAKE_SOURCE_DIR}/src/transport/library-loader.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/ndi/ndi-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-api.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/transport.cpp"
)
target_include_directories(
  ndi-backend-test
  PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/lib/ndi" "${CMAKE_SOURCE_DIR}/lib/omt"
)
target_compile_features(ndi-backend-test PRIVATE cxx_std_17)
target_link_libraries(ndi-backend-test PRIVATE OBS::libobs ${CMAKE_DL_LIBS})
add_dependencies(ndi-backend-test fake-ndi)

add_test(NAME ndi-backend COMMAND ndi-backend-test)
set_tests_properties(ndi-backend PROPERTIES ENVIRONMENT "NDI_RUNTIME_DIR_V6=${CMAKE_CURRENT_BINARY_DIR}/runtime")

add_executable(
  sender-test
  sender-test.cpp
  "${CMAKE_SOURCE_DIR}/src/metrics/feed-registry.cpp"
  "${CMAKE_SOURCE_DIR}/src/obs/sender-session.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/library-loader.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/ndi/ndi-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-api.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/transport.cpp"
)
target_include_directories(
  sender-test
  PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/lib/ndi" "${CMAKE_SOURCE_DIR}/lib/omt"
)
target_compile_features(sender-test PRIVATE cxx_std_17)
target_link_libraries(sender-test PRIVATE OBS::libobs ${CMAKE_DL_LIBS})
add_dependencies(sender-test fake-ndi)

add_test(NAME sender COMMAND sender-test)
set_tests_properties(sender PROPERTIES ENVIRONMENT "NDI_RUNTIME_DIR_V6=${CMAKE_CURRENT_BINARY_DIR}/runtime")

# Built as libomt next to the test binaries, so the backend's real "bundled beside the
# plugin" search path is what finds it - no environment variable involved.
add_library(fake-omt SHARED fake-omt.cpp)
target_include_directories(fake-omt PRIVATE "${CMAKE_SOURCE_DIR}/lib/omt")
set_target_properties(
  fake-omt
  PROPERTIES OUTPUT_NAME omt PREFIX "lib" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

add_executable(
  omt-test
  omt-test.cpp
  "${CMAKE_SOURCE_DIR}/src/metrics/feed-registry.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/library-loader.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/ndi/ndi-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-api.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/omt/omt-backend.cpp"
  "${CMAKE_SOURCE_DIR}/src/transport/transport.cpp"
)
target_include_directories(
  omt-test
  PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/lib/ndi" "${CMAKE_SOURCE_DIR}/lib/omt"
)
target_compile_features(omt-test PRIVATE cxx_std_17)
target_link_libraries(omt-test PRIVATE OBS::libobs ${CMAKE_DL_LIBS})
add_dependencies(omt-test fake-omt)

add_test(NAME omt COMMAND omt-test)
