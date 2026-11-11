add_executable(chronos_core_tests core/test_order.cpp orderbook/test_order_book.cpp)
target_link_libraries(chronos_core_tests PRIVATE chronos_core GTest::gtest_main)
chronos_configure_target(chronos_core_tests)
include(GoogleTest)
gtest_discover_tests(chronos_core_tests)

# Isolate the allocation-failure injector from the rest of the test process.
add_executable(chronos_allocation_tests orderbook/test_allocation_failure.cpp)
target_link_libraries(chronos_allocation_tests PRIVATE chronos_core GTest::gtest_main)
chronos_configure_target(chronos_allocation_tests)
gtest_discover_tests(chronos_allocation_tests)
