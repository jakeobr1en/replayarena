include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Do not let googletest inherit our -Werror set or install rules.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# PRE_TEST: discover at ctest time, not build time. Sanitizer-instrumented
# binaries can exceed the 5s build-time discovery timeout on first launch.
# TIMEOUT acts as the watchdog for deadlock-prone concurrency tests.
function(replayarena_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE replayarena_gateway replayarena_options GTest::gtest_main)
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    PROPERTIES TIMEOUT 120)
endfunction()

replayarena_add_test(request_queue_test)
replayarena_add_test(request_queue_stress_test)
