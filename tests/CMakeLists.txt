include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
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