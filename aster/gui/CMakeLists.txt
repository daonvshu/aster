find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_library(aster_gui STATIC imagebox.cpp imagebox.h gui.h)
add_library(aster::gui ALIAS aster_gui)
set_target_properties(aster_gui PROPERTIES AUTOMOC ON)
target_link_libraries(aster_gui PUBLIC aster::cache Qt${QT_VERSION_MAJOR}::Widgets)
