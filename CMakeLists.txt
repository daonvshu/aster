cmake_minimum_required(VERSION 3.16)
project(aster VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

option(ASTER_ENABLE_SQLITE_DISK_CACHE "Build the optional SQLite disk cache backend" ON)

add_subdirectory(aster/cache)

option(ASTER_BUILD_GUI "Build the GUI module for ImageBox" ON)
if(ASTER_BUILD_GUI)
    add_subdirectory(aster/gui)
endif()

option(ASTER_BUILD_GALLERY "Build the ImageBox gallery application" ON)
if(ASTER_BUILD_GUI AND ASTER_BUILD_GALLERY)
    add_subdirectory(examples/gallery)
endif()

option(ASTER_BUILD_CACHE_WIDGET "Build the cache-only algorithm comparison example" ON)
if(ASTER_BUILD_CACHE_WIDGET)
    add_subdirectory(examples/cache_widget)
endif()

# Compatibility build target; new consumers link aster::cache or aster::gui.
add_custom_target(aster DEPENDS aster_cache)

if(BUILD_TESTING)
    if(ASTER_BUILD_GUI)
        find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
        add_executable(aster_gui_tests
            tests/imagebox_tests.cpp
            tests/resources/source_resources.qrc)

        set_target_properties(aster_gui_tests PROPERTIES AUTOMOC ON AUTORCC ON)
        target_link_libraries(aster_gui_tests PRIVATE aster::gui Qt${QT_VERSION_MAJOR}::Test)

        add_test(NAME aster_gui_tests COMMAND aster_gui_tests)
        set_tests_properties(aster_gui_tests PROPERTIES TIMEOUT 120
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>/../plugins")
        if(WIN32)
            set_property(TEST aster_gui_tests APPEND PROPERTY ENVIRONMENT
                "PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>\\;$ENV{PATH}")
        endif()
    endif()

    set(ASTER_TEST_SOURCES
        tests/cache_tests.cpp
        tests/second_round_tests.cpp
        tests/third_round_tests.cpp
        tests/fourth_round_tests.cpp
        tests/imageservice_tests.cpp
        tests/friendly_request_tests.cpp
        tests/resampler_tests.cpp
        tests/transformation_tests.cpp
        tests/decoder_tests.cpp

        tests/sqlite_disk_cache_tests.cpp

        tests/reference/picasaresampler.cpp

        tests/resources/source_resources.qrc)

    if(NOT ASTER_ENABLE_SQLITE_DISK_CACHE)
        list(REMOVE_ITEM ASTER_TEST_SOURCES tests/sqlite_disk_cache_tests.cpp)
    endif()

    add_executable(aster_tests ${ASTER_TEST_SOURCES})
    set_target_properties(aster_tests PROPERTIES AUTORCC ON)
    target_link_libraries(aster_tests PRIVATE aster::cache)

    if(ASTER_BUILD_QT_NETWORK)
        target_link_libraries(aster_tests PRIVATE aster::network)
        target_compile_definitions(aster_tests PRIVATE ASTER_TEST_QT_NETWORK)
        option(ASTER_BUILD_HTTP_TESTS "Build Python-backed HTTP integration tests" ON)

        if(ASTER_BUILD_GUI AND ASTER_BUILD_HTTP_TESTS)
            add_subdirectory(tests/http_server)
        endif()
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

set(ASTER_PACKAGE_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/aster")
set(ASTER_PACKAGE_HAS_GUI ${ASTER_BUILD_GUI})
set(ASTER_PACKAGE_HAS_NETWORK ${ASTER_BUILD_QT_NETWORK})
set(ASTER_PACKAGE_HAS_SQLITE ${ASTER_ENABLE_SQLITE_DISK_CACHE})

set_target_properties(aster_cache PROPERTIES EXPORT_NAME cache)
if(ASTER_BUILD_GUI)
    set_target_properties(aster_gui PROPERTIES EXPORT_NAME gui)
endif()
if(ASTER_BUILD_QT_NETWORK)
    set_target_properties(aster_network PROPERTIES EXPORT_NAME network)
endif()

if(ASTER_BUILD_GUI)
    target_include_directories(aster_gui PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endif()

install(TARGETS aster_cache
    EXPORT asterTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(ASTER_BUILD_GUI)
    install(TARGETS aster_gui
        EXPORT asterTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(ASTER_BUILD_QT_NETWORK)
    install(TARGETS aster_network
        EXPORT asterTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/aster/cache/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aster/cache
    FILES_MATCHING PATTERN "*.h")

if(ASTER_BUILD_GUI)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/aster/gui/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aster/gui
        FILES_MATCHING PATTERN "*.h"
        PATTERN "private" EXCLUDE)
endif()

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/asterConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/asterConfig.cmake
    INSTALL_DESTINATION ${ASTER_PACKAGE_CONFIG_DIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/asterConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(EXPORT asterTargets
    FILE asterTargets.cmake
    NAMESPACE aster::
    DESTINATION ${ASTER_PACKAGE_CONFIG_DIR})

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/asterConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/asterConfigVersion.cmake
    DESTINATION ${ASTER_PACKAGE_CONFIG_DIR})

option(ASTER_BUILD_BENCHMARKS "Build the cache benchmark" ON)
if(ASTER_BUILD_BENCHMARKS)
    add_executable(aster_benchmark
        benchmarks/cache_benchmark.cpp
        benchmarks/disk_benchmark.cpp
        benchmarks/rendered_benchmark.cpp
        benchmarks/decoder_benchmark.cpp
        benchmarks/resampler_benchmark.cpp)

    target_link_libraries(aster_benchmark PRIVATE aster::cache)
endif()
