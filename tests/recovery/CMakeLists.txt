set(RECOVERY_CPU_TESTS
    background_rebuild_manager
    shutdown_completion
    config_uint8_numeric
    ec_batch_pending_backpressure
    ec_batch_worker_private
    ec_diagnostic_counter
    ec_isal_codec
    ec_update_participant
    ec_slot_reuse
    ec_rmw_read_fallback
    ec_direct_codec
    ec_direct_write_bank
    ec_direct_group
    ec_direct_size_classes
    ec_size_class_builder
    ec_direct_source_gate
    evict_breakdown
    ec_space_accounting
    ec_space_reporting
    ec_read_context
    ec_read_four_read
    ec_recovery_scratch_growth
    ec_split_foundation
    ec_split_manager
    entry_frequency
    entry_frequency_lifecycle
    entry_frequency_oom
    recovery_stats_wordcount
    recomputable_entry
    recompute_recipe)

foreach(test_name IN LISTS RECOVERY_CPU_TESTS)
    add_executable(test_${test_name} ${test_name}.cpp)
    target_sources(test_${test_name} PRIVATE $<TARGET_OBJECTS:cpu_profile_state>)
    target_compile_options(test_${test_name} PRIVATE -UNDEBUG)
    target_link_libraries(test_${test_name} PRIVATE Threads::Threads ${ISAL_LIBRARY})
    add_test(NAME recovery_${test_name} COMMAND test_${test_name})
    set_tests_properties(recovery_${test_name} PROPERTIES TIMEOUT 60)
endforeach()

target_include_directories(test_recovery_stats_wordcount PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/include")

# Count real coefficient expansions in the test, not in the production hot path.
target_link_options(test_ec_isal_codec PRIVATE -Wl,--wrap=ec_init_tables)
set_tests_properties(recovery_evict_breakdown PROPERTIES SKIP_RETURN_CODE 77)

# This test exercises the real allocator helpers and their real runtime deps.
add_executable(test_exclusive_work_alignment exclusive_work_alignment.cpp)
target_compile_options(test_exclusive_work_alignment PRIVATE -UNDEBUG)
target_link_libraries(test_exclusive_work_alignment PRIVATE ${COMMON_LIBS})
add_test(NAME recovery_exclusive_work_alignment COMMAND test_exclusive_work_alignment)
set_tests_properties(recovery_exclusive_work_alignment PROPERTIES TIMEOUT 60)

add_executable(test_background_rebuild_control background_rebuild_control_test.cpp)
target_compile_options(test_background_rebuild_control PRIVATE -UNDEBUG)
target_link_libraries(test_background_rebuild_control PRIVATE Threads::Threads)
add_test(NAME recovery_background_rebuild_control COMMAND test_background_rebuild_control)
set_tests_properties(recovery_background_rebuild_control PROPERTIES TIMEOUT 60)
add_test(NAME recovery_background_rebuild_creation_race
         COMMAND test_background_rebuild_manager creation-race)
set_tests_properties(recovery_background_rebuild_creation_race PROPERTIES TIMEOUT 60)

add_executable(test_partition_recipes partition_recipes.cpp)
target_compile_options(test_partition_recipes PRIVATE -UNDEBUG)
target_link_libraries(test_partition_recipes PRIVATE ${COMMON_LIBS})
add_test(NAME recovery_partition_recipes COMMAND test_partition_recipes)
set_tests_properties(recovery_partition_recipes PROPERTIES TIMEOUT 30)

add_executable(test_ordinal_table ordinal_table.cpp)
target_include_directories(test_ordinal_table PRIVATE "${PROJECT_SOURCE_DIR}/runtime/include")
target_compile_options(test_ordinal_table PRIVATE -UNDEBUG)
target_link_libraries(test_ordinal_table PRIVATE ${COMMON_LIBS})
add_test(NAME recovery_ordinal_table COMMAND test_ordinal_table)
set_tests_properties(recovery_ordinal_table PROPERTIES TIMEOUT 30)

# Manual real-RDMA paper-style single-object recomputation fixture. It requires
# an external seven-endpoint harness, so it is intentionally not a CTest test.
add_executable(paper_object_recovery paper_object_fixture.cpp)
target_compile_options(paper_object_recovery PRIVATE -UNDEBUG)
target_link_libraries(paper_object_recovery PRIVATE ${COMMON_LIBS})
