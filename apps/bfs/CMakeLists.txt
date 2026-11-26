add_executable(gapbs_bfs_chunked gapbs_bfs_chunked.cpp)
target_include_directories(gapbs_bfs_chunked PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gapbs_bfs_chunked hdr_histogram::hdr_histogram ${COMMON_LIBS})

if(DEFINED STARFISH_BFS_OUTPUT_DIR)
    set_target_properties(gapbs_bfs_chunked
                          PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                     "${STARFISH_BFS_OUTPUT_DIR}")
endif()

if(STARFISH_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    add_executable(graph_loader_affinity_test tests/graph_loader_affinity.cpp)
    target_include_directories(graph_loader_affinity_test PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(graph_loader_affinity_test PRIVATE -UNDEBUG)
    target_link_libraries(graph_loader_affinity_test PRIVATE Threads::Threads)
    add_test(NAME bfs_graph_loader_affinity COMMAND graph_loader_affinity_test)
    set_tests_properties(bfs_graph_loader_affinity PROPERTIES TIMEOUT 90)

    add_executable(graph_input_regression_test tests/graph_input_probe.cpp)
    target_include_directories(graph_input_regression_test PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(graph_input_regression_test PRIVATE Threads::Threads)
    add_test(
        NAME bfs_graph_input_regression
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_input_regression.py"
                "$<TARGET_FILE:graph_input_regression_test>"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/prepare_graph.py")
    set_tests_properties(bfs_graph_input_regression PROPERTIES TIMEOUT 90)
endif()
