include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
  DOWNLOAD_EXTRACT_TIMESTAMP true
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ishap_tests test_ishap.cpp)

target_link_libraries(ishap_tests
    PRIVATE
        ishap::ishap
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(ishap_tests)