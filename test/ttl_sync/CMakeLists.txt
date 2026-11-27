add_executable(TtlSyncTest
  TtlSyncTest.cpp
  ${REPO_ROOT}/src/ttl/TtlSyncCore.cpp
)

target_include_directories(TtlSyncTest PRIVATE
  ${REPO_ROOT}/src
)

target_compile_definitions(TtlSyncTest PRIVATE
  TTL_REPO_ROOT="${REPO_ROOT}"
)

target_compile_options(TtlSyncTest PRIVATE
  -Wall
  -Wextra
  -pedantic
  -Werror
)

target_link_libraries(TtlSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TtlSyncTest)
