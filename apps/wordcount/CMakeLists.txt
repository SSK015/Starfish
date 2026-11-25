add_executable(wordcount_native wordcount_native.cpp)
target_link_libraries(wordcount_native Threads::Threads)

add_executable(word_length_stats word_length_stats.cpp)
target_link_libraries(word_length_stats Threads::Threads)

option(STARFISH_WORDCOUNT_RECOMPUTE "Enable partition recipes in Wordcount" ${STARFISH_WORDCOUNT_RUNTIME})
if(STARFISH_WORDCOUNT_RECOMPUTE AND NOT STARFISH_WORDCOUNT_RUNTIME)
    message(FATAL_ERROR "Wordcount recipes require the Starfish runtime")
endif()

add_executable(wordcount_far wordcount_far.cpp)
target_link_libraries(wordcount_far ${COMMON_LIBS})

if(STARFISH_WORDCOUNT_RECOMPUTE)
    add_executable(wordcount_recipes wordcount_recipes.cpp)
    target_link_libraries(wordcount_recipes ${COMMON_LIBS})
endif()

option(STARFISH_BUILD_WORDCOUNT_RECOVERY
       "Build the opt-in fixture-only Wordcount RDMA recovery regression" OFF)
if(STARFISH_BUILD_WORDCOUNT_RECOVERY)
    if(NOT STARFISH_WORDCOUNT_RUNTIME OR NOT STARFISH_WORDCOUNT_RECOMPUTE)
        message(FATAL_ERROR "Wordcount recovery tests require the Starfish runtime and partition recipes")
    endif()
    add_executable(
        wordcount_recovery
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/recovery/wordcount_recipe_fixture.cpp")
    target_include_directories(wordcount_recovery
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(wordcount_recovery PRIVATE ${COMMON_LIBS})
endif()

set(_wordcount_targets wordcount_native wordcount_far word_length_stats)
if(TARGET wordcount_recipes)
    list(APPEND _wordcount_targets wordcount_recipes)
endif()
add_custom_target(wordcount_apps DEPENDS ${_wordcount_targets})

if(DEFINED STARFISH_WORDCOUNT_OUTPUT_DIR)
    if(TARGET wordcount_recovery)
        list(APPEND _wordcount_targets wordcount_recovery)
    endif()
    foreach(target IN LISTS _wordcount_targets)
        set_target_properties(${target}
                              PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                         "${STARFISH_WORDCOUNT_OUTPUT_DIR}")
    endforeach()
endif()
