cmake_minimum_required(VERSION 3.25)

project(
        SkyUIInventoryOptimizer
        VERSION 0.0.1
        LANGUAGES CXX)
set(PROJECT_LICENSE "CC0-1.0")
set(PROJECT_FRIENDLY_NAME ${PROJECT_NAME})
set(PROJECT_LONG_NAME "SkyUI Inventory Optimizer")
set(PROJECT_DESCRIPTION ${PROJECT_LONG_NAME})
set(PROJECT_COPYRIGHT "Copyright (c) 2025 Fengzi2333")
set(PROJECT_AUTHOR "Fengzi2333")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(addpluginsources)
include(pluginconfig)
include(plugintarget)
include(pluginpackage)
cmake_language(DEFER CALL include pluginbuildoptions)
include("${PROJECT_SOURCE_DIR}/local.cmake" OPTIONAL)

######## dependencies
set(Boost_NO_WARN_NEW_VERSIONS 1)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED  ON)
find_package(Boost CONFIG COMPONENTS)
if(Boost_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${Boost_LIBRARIES})
    target_include_directories(${PROJECT_NAME} PRIVATE ${Boost_INCLUDE_DIRS})
endif()

find_library(DETOURS_LIBRARY detours
    PATHS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
    NO_DEFAULT_PATH
)

if(DETOURS_LIBRARY)
    message("-- Found detours")
    target_include_directories(${PROJECT_NAME} PRIVATE ${DETOURS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DETOURS_LIBRARY})
else()
    message("-- Could NOT find detours")
endif()

######## profiler (Tracy)  (optional)
# See docs/tracy-integration-plan.md section 2.2.
# Tracy lives in the git-ignored extern/ directory, so source code never
# includes <tracy/Tracy.hpp> unconditionally -- everything goes through
# src/Profiling.h, which degrades to no-op macros when TRACY_ENABLE is absent.
option(ENABLE_TRACY "Build with Tracy Profiler instrumentation" OFF)

if(ENABLE_TRACY)
    find_path(TracyPath "CMakeLists.txt"
        PATHS   "${CMAKE_SOURCE_DIR}/extern/TracyProfiler"
                "${CMAKE_SOURCE_DIR}/external/TracyProfiler" NO_DEFAULT_PATH)

    if(EXISTS "${TracyPath}")
        # Must be set BEFORE add_subdirectory: Tracy declares these with option(),
        # which does not override an already-cached value.
        set(TRACY_ENABLE ON CACHE BOOL "Enable the Tracy client" FORCE)
        # Collect data only while the profiler GUI is connected.
        set(TRACY_ON_DEMAND ON CACHE BOOL "Tracy: collect only while GUI is connected" FORCE)
        # Keep the SKSE crash logger in charge of crash reporting.
        set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "Tracy: disable its own crash handler" FORCE)

        add_subdirectory("${TracyPath}" TracyProfiler EXCLUDE_FROM_ALL)
        # Tracy's options are PUBLIC (cmake/options.cmake), so linking TracyClient
        # also defines TRACY_ENABLE for this target.
        target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
        message("-- Tracy Profiler enabled (${TracyPath})")
    else()
        message(WARNING "-- ENABLE_TRACY=ON but extern/TracyProfiler was not found; profiling disabled")
    endif()
endif()
####################