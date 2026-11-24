add_executable(chronos_core_tests core/test_order.cpp orderbook/test_order_book.cpp matching/test_matching.cpp matching/test_differential.cpp exchange/test_exchange.cpp exchange/test_id_allocator.cpp exchange/test_allocation_registration.cpp exchange/test_transaction_boundary.cpp exchange/test_offline_owner.cpp exchange/test_live_owner.cpp exchange/test_prepared.cpp exchange/test_installed_summary.cpp exchange/test_canonical_command.cpp simulation/test_simulation.cpp eventlog/test_event_log.cpp snapshot/test_checkpoint.cpp snapshot/test_restorable.cpp journal/test_inspection.cpp)
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

add_executable(chronos_prepared_exhaustion_tests exchange/test_prepared_exhaustion.cpp)
target_link_libraries(chronos_prepared_exhaustion_tests PRIVATE chronos_core_id_test GTest::gtest_main)
chronos_configure_target(chronos_prepared_exhaustion_tests)
gtest_discover_tests(chronos_prepared_exhaustion_tests PROPERTIES TIMEOUT 10)

add_executable(chronos_capture_allocation_tests exchange/test_capture_allocation.cpp)
target_link_libraries(chronos_capture_allocation_tests PRIVATE chronos_core_id_test GTest::gtest_main)
chronos_configure_target(chronos_capture_allocation_tests)
gtest_discover_tests(chronos_capture_allocation_tests PROPERTIES TIMEOUT 10)

if(TARGET chronos_gateway_lib)
  add_executable(chronos_gateway_tests gateway/test_gateway.cpp)
  target_link_libraries(chronos_gateway_tests PRIVATE chronos_gateway_lib GTest::gtest_main)
  chronos_configure_target(chronos_gateway_tests)
  gtest_discover_tests(chronos_gateway_tests PROPERTIES TIMEOUT 10)

  # Separate gateway copy confines the blocking pre-install test hook to this
  # executable; the production gateway does not compile the hook.
  add_library(chronos_gateway_gate_test STATIC ../src/gateway/protocol.cpp ../src/gateway/tcp.cpp)
  target_compile_definitions(chronos_gateway_gate_test PUBLIC CHRONOS_TEST_GATE_SEAM)
  target_link_libraries(chronos_gateway_gate_test PUBLIC Threads::Threads chronos_core)
  chronos_configure_target(chronos_gateway_gate_test)
  add_executable(chronos_gateway_gate_tests gateway/test_publication_gate.cpp)
  target_link_libraries(chronos_gateway_gate_tests PRIVATE chronos_gateway_gate_test GTest::gtest_main)
  chronos_configure_target(chronos_gateway_gate_tests)
  gtest_discover_tests(chronos_gateway_gate_tests PROPERTIES TIMEOUT 10)
endif()
