cmake_minimum_required(VERSION 3.16)
project(aster VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(ASTER_QT_MAJOR "" CACHE STRING "Qt major version (5 or 6); empty prefers Qt6")
set_property(CACHE ASTER_QT_MAJOR PROPERTY STRINGS "" 5 6)
if(ASTER_QT_MAJOR)
    if(NOT ASTER_QT_MAJOR MATCHES "^[56]$")
        message(FATAL_ERROR "ASTER_QT_MAJOR must be 5 or 6")
    endif()
    find_package(Qt${ASTER_QT_MAJOR} REQUIRED COMPONENTS Core Gui)
    set(QT_VERSION_MAJOR ${ASTER_QT_MAJOR})
else()
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)
endif()
if(QT_VERSION_MAJOR EQUAL 5 AND Qt5Core_VERSION VERSION_LESS 5.12)
    message(FATAL_ERROR "aster requires Qt 5.12 or newer")
endif()

find_package(Threads REQUIRED)
include(CTest)

add_subdirectory(aster/cache)

option(ASTER_BUILD_GUI "Build the GUI module for ImageBox" ON)
if(ASTER_BUILD_GUI)
    add_subdirectory(aster/gui)
endif()

# Compatibility build target; new consumers link aster::cache or aster::gui.
add_custom_target(aster DEPENDS aster_cache)

if(BUILD_TESTING)
    add_executable(aster_tests tests/cache_tests.cpp tests/second_round_tests.cpp
        tests/third_round_tests.cpp tests/fourth_round_tests.cpp tests/imageservice_tests.cpp
        tests/friendly_request_tests.cpp tests/resources/source_resources.qrc)
    set_target_properties(aster_tests PROPERTIES AUTORCC ON)
    target_link_libraries(aster_tests PRIVATE aster::cache)
    if(ASTER_BUILD_QT_NETWORK)
        target_link_libraries(aster_tests PRIVATE aster::network)
        target_compile_definitions(aster_tests PRIVATE ASTER_TEST_QT_NETWORK)
    endif()
    add_test(NAME aster_tests COMMAND aster_tests)
    set_tests_properties(aster_tests PROPERTIES TIMEOUT 120)
    add_executable(aster_soak tests/cache_soak.cpp)
    target_link_libraries(aster_soak PRIVATE aster::cache)
    if(WIN32)
        target_link_libraries(aster_soak PRIVATE Psapi)
    endif()
    add_test(NAME aster_soak_smoke COMMAND aster_soak --iterations 3000)
    set_tests_properties(aster_soak_smoke PROPERTIES TIMEOUT 180 LABELS soak)
    if(WIN32)
        string(REPLACE ";" "\\;" aster_test_path
            "$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>;$ENV{PATH}")
        set_tests_properties(aster_tests aster_soak_smoke PROPERTIES
            ENVIRONMENT "PATH=${aster_test_path}")
    endif()
    add_test(NAME aster_key_stability COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:aster_tests>
        -DQT_RUNTIME_DIR=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/key_stability.cmake)
endif()
option(ASTER_BUILD_BENCHMARKS "Build the cache benchmark" ON)
if(ASTER_BUILD_BENCHMARKS)
    add_executable(aster_benchmark benchmarks/cache_benchmark.cpp benchmarks/disk_benchmark.cpp
        benchmarks/rendered_benchmark.cpp benchmarks/decoder_benchmark.cpp)
    target_link_libraries(aster_benchmark PRIVATE aster::cache)
endif()
