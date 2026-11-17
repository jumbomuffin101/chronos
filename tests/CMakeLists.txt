add_executable(chronos_core_tests core/test_order.cpp orderbook/test_order_book.cpp matching/test_matching.cpp matching/test_differential.cpp exchange/test_exchange.cpp exchange/test_id_allocator.cpp simulation/test_simulation.cpp eventlog/test_event_log.cpp snapshot/test_checkpoint.cpp snapshot/test_restorable.cpp)
target_link_libraries(chronos_core_tests PRIVATE chronos_core GTest::gtest_main)
target_compile_definitions(chronos_core_tests PRIVATE CHRONOS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
chronos_configure_target(chronos_core_tests)
include(GoogleTest)
gtest_discover_tests(chronos_core_tests)

# Isolate the allocation-failure injector from the rest of the test process.
add_executable(chronos_allocation_tests orderbook/test_allocation_failure.cpp)
target_link_libraries(chronos_allocation_tests PRIVATE chronos_core GTest::gtest_main)
chronos_configure_target(chronos_allocation_tests)
gtest_discover_tests(chronos_allocation_tests)

if(TARGET chronos_gateway_lib)
  add_executable(chronos_gateway_tests gateway/test_gateway.cpp)
  target_link_libraries(chronos_gateway_tests PRIVATE chronos_gateway_lib GTest::gtest_main)
  chronos_configure_target(chronos_gateway_tests)
  gtest_discover_tests(chronos_gateway_tests PROPERTIES TIMEOUT 10)
endif()
