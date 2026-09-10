find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(NAME aster_http_server_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_server.py)
set_tests_properties(aster_http_server_tests PROPERTIES TIMEOUT 30)
if(TARGET aster_gallery_tests)
    add_test(NAME aster_gallery_http_tests COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py -- $<TARGET_FILE:aster_gallery_tests>
        httpPanelIntegration -o ${CMAKE_CURRENT_BINARY_DIR}/gallery-http-results.txt,txt)
    set_tests_properties(aster_gallery_http_tests PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>/../plugins")
    if(WIN32)
        set_property(TEST aster_gallery_http_tests APPEND PROPERTY ENVIRONMENT
            "PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>\\;$ENV{PATH}")
    endif()
endif()
add_executable(aster_http_tests ../http_integration_tests.cpp)
set_target_properties(aster_http_tests PROPERTIES AUTOMOC ON)
target_link_libraries(aster_http_tests PRIVATE aster::gui aster::network Qt${QT_VERSION_MAJOR}::Test)
add_test(NAME aster_http_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py -- $<TARGET_FILE:aster_http_tests>
    -o ${CMAKE_CURRENT_BINARY_DIR}/http-results.txt,txt)
set_tests_properties(aster_http_tests PROPERTIES TIMEOUT 120 LABELS integration
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>/../plugins")
if(WIN32)
    set_property(TEST aster_http_tests APPEND PROPERTY ENVIRONMENT
        "PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>\\;$ENV{PATH}")
endif()
