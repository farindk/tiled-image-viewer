cmake_minimum_required(VERSION 3.10)
project(tiled_image_viewer)

find_package(PkgConfig)

# Download and compile Raylib

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "5.5"
    GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(raylib)

# Find libheif

pkg_check_modules(LIBHEIF REQUIRED libheif)

# Find libcurl for HTTP support
find_package(CURL REQUIRED)

# Executable

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE sources/main.cc sources/http_reader.cc)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INCLUDE})
target_link_libraries(${PROJECT_NAME} PRIVATE raylib)

target_include_directories(${PROJECT_NAME} PRIVATE ${LIBHEIF_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${LIBHEIF_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBHEIF_LIBRARIES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CURL_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl)
