# Keep the bundled source immutable; generate headers and libraries in the build tree.
set(LIBFIBRE_DIR "" CACHE PATH
    "Optional prebuilt external libfibre checkout; empty builds the bundled source")

if(LIBFIBRE_DIR)
    get_filename_component(LIBFIBRE_DIR "${LIBFIBRE_DIR}" ABSOLUTE)
    if(NOT EXISTS "${LIBFIBRE_DIR}/src/libfibre/fibre.h")
        message(FATAL_ERROR "LIBFIBRE_DIR must contain src/libfibre/fibre.h")
    endif()
    unset(FARLIB_EXTERNAL_FIBRE_LIBRARY CACHE)
    find_library(FARLIB_EXTERNAL_FIBRE_LIBRARY NAMES fibre
                 PATHS "${LIBFIBRE_DIR}/src" NO_DEFAULT_PATH)
    if(NOT FARLIB_EXTERNAL_FIBRE_LIBRARY)
        message(FATAL_ERROR "Build external libfibre first, or leave LIBFIBRE_DIR empty")
    endif()
    set(FARLIB_FIBRE_INCLUDE_DIRS "${LIBFIBRE_DIR}/src")
    add_library(farlib_libfibre SHARED IMPORTED GLOBAL)
    set_target_properties(farlib_libfibre PROPERTIES
        IMPORTED_LOCATION "${FARLIB_EXTERNAL_FIBRE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FARLIB_FIBRE_INCLUDE_DIRS}")
    message(STATUS "Using external libfibre: ${FARLIB_EXTERNAL_FIBRE_LIBRARY}")
else()
    get_filename_component(_fibre_root
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/libfibre" ABSOLUTE)
    set(_fibre_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/libfibre")
    file(MAKE_DIRECTORY "${_fibre_generated}/runtime-glue")
    configure_file(
        "${_fibre_root}/src/runtime-glue/testoptions.h.default"
        "${_fibre_generated}/runtime-glue/testoptions.h" COPYONLY)

    # The source list and default flags follow the pinned src/Makefile and
    # Makefile.config. Optional tracing/split-stack modes are not enabled.
    add_library(farlib_libfibre SHARED
        "${_fibre_root}/src/runtime/BaseProcessor.cc"
        "${_fibre_root}/src/runtime/Debug.cc"
        "${_fibre_root}/src/runtime/Fred.cc"
        "${_fibre_root}/src/runtime/Stats.cc"
        "${_fibre_root}/src/runtime/Stack.S"
        "${_fibre_root}/src/libfibre/Bootstrap.cc"
        "${_fibre_root}/src/libfibre/Cluster.cc"
        "${_fibre_root}/src/libfibre/Fibre.cc"
        "${_fibre_root}/src/libfibre/Poller.cc"
        "${_fibre_root}/src/libfibre/cfibre.cc"
        "${_fibre_root}/src/errnoname/errnoname.c")
    set_target_properties(farlib_libfibre PROPERTIES
        OUTPUT_NAME fibre
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/third_party/libfibre"
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(farlib_libfibre SYSTEM PUBLIC
        "${_fibre_root}/src" "${_fibre_generated}")
    target_compile_definitions(farlib_libfibre PRIVATE __LIBFIBRE__)
    # Upstream's optimized build keeps debug symbols and does not set NDEBUG.
    target_compile_options(farlib_libfibre PRIVATE
        -Wall -Wextra -O3 -ggdb -fno-omit-frame-pointer -pthread -UNDEBUG)
    target_link_libraries(farlib_libfibre PRIVATE Threads::Threads)
    target_link_options(farlib_libfibre PRIVATE -pthread)
    set(FARLIB_FIBRE_INCLUDE_DIRS "${_fibre_root}/src" "${_fibre_generated}")
    message(STATUS "Building bundled libfibre: ${_fibre_root}")
endif()
