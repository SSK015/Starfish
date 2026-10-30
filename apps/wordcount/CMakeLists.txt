add_executable(wordcount_native wordcount_native.cpp)
target_link_libraries(wordcount_native Threads::Threads)

add_executable(word_length_stats word_length_stats.cpp)
target_link_libraries(word_length_stats Threads::Threads)

add_executable(wordcount_far wordcount_far.cpp)
target_link_libraries(wordcount_far ${COMMON_LIBS})
option(STARFISH_WORDCOUNT_RECOMPUTE "Compile Starfish recomputation hooks in shared WC" ${STARFISH_WORDCOUNT_RUNTIME})
if(STARFISH_WORDCOUNT_RECOMPUTE AND NOT STARFISH_WORDCOUNT_RUNTIME)
    message(FATAL_ERROR "Starfish Wordcount hooks require the Starfish runtime")
endif()
target_compile_definitions(wordcount_far PRIVATE
    STARFISH_WORDCOUNT_RECOMPUTE=$<BOOL:${STARFISH_WORDCOUNT_RECOMPUTE}>
    STARFISH_WORDCOUNT_RECOVERY_TEST=0)

option(STARFISH_BUILD_WORDCOUNT_RECOVERY "Build Wordcount recovery validation" OFF)
if(STARFISH_BUILD_WORDCOUNT_RECOVERY)
    if(NOT STARFISH_WORDCOUNT_RUNTIME OR NOT STARFISH_WORDCOUNT_RECOMPUTE)
        message(FATAL_ERROR
                "STARFISH_BUILD_WORDCOUNT_RECOVERY requires a runtime with the "
                "Starfish build enabled")
    endif()
    add_executable(wordcount_recovery wordcount_far.cpp)
    target_link_libraries(wordcount_recovery ${COMMON_LIBS})
    target_compile_definitions(wordcount_recovery PRIVATE
        STARFISH_WORDCOUNT_RECOMPUTE=1 STARFISH_WORDCOUNT_RECOVERY_TEST=1)
endif()

if(DEFINED STARFISH_WORDCOUNT_OUTPUT_DIR)
    set(_wordcount_targets wordcount_native word_length_stats wordcount_far)
    if(TARGET wordcount_recovery)
        list(APPEND _wordcount_targets wordcount_recovery)
    endif()
    foreach(target IN LISTS _wordcount_targets)
        set_target_properties(${target}
                              PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                         "${STARFISH_WORDCOUNT_OUTPUT_DIR}")
    endforeach()
endif()
