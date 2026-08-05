include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Do not let googletest inherit our -Werror set or install rules.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE replayarena_options GTest::gtest_main)

# PRE_TEST: discover at ctest time, not build time. Sanitizer-instrumented
# binaries can exceed the 5s build-time discovery timeout on first launch.
include(GoogleTest)
gtest_discover_tests(smoke_test DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
