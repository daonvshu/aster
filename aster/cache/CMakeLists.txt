add_library(aster_cache STATIC
    core/imagekey.cpp
    core/imagerendergeometry.cpp
    renderer/imageresampler.cpp
    renderer/imagerenderer.cpp
    renderer/imagetransformation.cpp
    decoder/boundedimagedecoder.cpp
    cache/renderedmemorycache.cpp
    cache/encodedmemorycache.cpp
    cache/filediskcache.cpp
    cache/rendereddiskcache.cpp
    pipeline/activeresourcestore.cpp
    pipeline/pipelineinterceptor.cpp
    source/cachedsourceloader.cpp
    source/httpinterceptor.cpp
    source/imagesource.cpp
    source/sourceinterceptor.cpp
    pipeline/imagesubscription.cpp
    pipeline/imagesubscription.h
    service/imageservice.cpp
    pipeline/imagepipeline.cpp)
add_library(aster::cache ALIAS aster_cache)
option(ASTER_LANCZOS_USE_LUT "Use approximate integer Lanczos kernel tables by default" OFF)
target_compile_definitions(aster_cache PUBLIC ASTER_LANCZOS_USE_LUT=$<BOOL:${ASTER_LANCZOS_USE_LUT}>)
add_library(aster::aster ALIAS aster_cache)
set_target_properties(aster_cache PROPERTIES AUTOMOC ON)
target_include_directories(aster_cache PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>)
target_link_libraries(aster_cache PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Threads::Threads)
target_compile_features(aster_cache PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(aster_cache PUBLIC /utf-8 /Zc:__cplusplus /permissive- PRIVATE /W4)
else()
    target_compile_options(aster_cache PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(ASTER_SANITIZER "" CACHE STRING "Sanitizer: address or thread (GCC/Clang)")
if(ASTER_SANITIZER)
    if(MSVC OR NOT ASTER_SANITIZER MATCHES "^(address|thread)$")
        message(FATAL_ERROR "Use address or thread with GCC/Clang")
    endif()
    target_compile_options(aster_cache PUBLIC -fsanitize=${ASTER_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(aster_cache PUBLIC -fsanitize=${ASTER_SANITIZER})
endif()

option(ASTER_BUILD_QT_NETWORK "Build optional QtNetwork transport and local HTTP tests" ON)
if(ASTER_BUILD_QT_NETWORK)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Network)
    add_library(aster_network STATIC source/qtnetworkservice.cpp)
    add_library(aster::network ALIAS aster_network)
    target_link_libraries(aster_network PUBLIC aster::cache Qt${QT_VERSION_MAJOR}::Network)
endif()
