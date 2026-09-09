find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# ImageBox will turn this scaffold into a compiled GUI library.
add_library(aster_gui INTERFACE)
add_library(aster::gui ALIAS aster_gui)

target_sources(aster_gui INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gui.h>)
target_link_libraries(aster_gui INTERFACE aster::cache Qt${QT_VERSION_MAJOR}::Widgets)

