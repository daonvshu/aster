find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Concurrent)

add_library(aster_gallery_ui STATIC
    imagecatalog.cpp
    imagecatalog.h
    imagegrid.cpp
    imagegrid.h
    gallerywindow.cpp
    gallerywindow.h
    httptestpanel.cpp
    httptestpanel.h
    smoothscrollbar.cpp
    smoothscrollbar.h
)

set_target_properties(aster_gallery_ui PROPERTIES AUTOMOC ON)
target_link_libraries(aster_gallery_ui PUBLIC aster::gui Qt${QT_VERSION_MAJOR}::Concurrent)

add_executable(aster_gallery WIN32 main.cpp)
target_link_libraries(aster_gallery PRIVATE aster_gallery_ui)

if(ASTER_BUILD_QT_NETWORK)
    target_link_libraries(aster_gallery PRIVATE aster::network)
    target_compile_definitions(aster_gallery PRIVATE ASTER_GALLERY_NETWORK)
endif()

if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

    add_executable(aster_gallery_tests gallery_tests.cpp)
    set_target_properties(aster_gallery_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(aster_gallery_tests PRIVATE aster_gallery_ui Qt${QT_VERSION_MAJOR}::Test)

    if(ASTER_BUILD_QT_NETWORK)
        target_link_libraries(aster_gallery_tests PRIVATE aster::network)
        target_compile_definitions(aster_gallery_tests PRIVATE ASTER_GALLERY_NETWORK)
    endif()

    add_test(NAME aster_gallery_tests COMMAND aster_gallery_tests)
    set_tests_properties(aster_gallery_tests PROPERTIES TIMEOUT 120
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>/../plugins")
    if(WIN32)
        set_property(TEST aster_gallery_tests APPEND PROPERTY ENVIRONMENT
            "PATH=$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>\\;$ENV{PATH}")
    endif()
endif()
