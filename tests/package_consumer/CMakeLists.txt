cmake_minimum_required(VERSION 3.16)
project(aster_package_consumer LANGUAGES CXX)

find_package(aster CONFIG REQUIRED)

add_executable(aster_package_consumer main.cpp)
target_link_libraries(aster_package_consumer PRIVATE aster::cache aster::gui)
target_compile_features(aster_package_consumer PRIVATE cxx_std_17)
