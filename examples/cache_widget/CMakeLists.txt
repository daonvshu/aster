find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(aster_cache_widget WIN32 main.cpp cachewidget.cpp cachewidget.h)
set_target_properties(aster_cache_widget PROPERTIES AUTOMOC ON)
target_link_libraries(aster_cache_widget PRIVATE aster::cache Qt${QT_VERSION_MAJOR}::Widgets)
