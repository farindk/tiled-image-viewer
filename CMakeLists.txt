cmake_minimum_required(VERSION 3.10)
project(tiled_image_viewer)

find_package(PkgConfig)

# Download and compile Raylib

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "master"
    GIT_PROGRESS TRUE
)

FetchContent_MakeAvailable(raylib)

# Find libheif

pkg_check_modules(LIBHEIF REQUIRED libheif)

# Executable

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE sources/main.cc)
target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_INCLUDE})
target_link_libraries(${PROJECT_NAME} PRIVATE raylib)

target_include_directories(${PROJECT_NAME} PRIVATE ${LIBHEIF_INCLUDE_DIRS})
target_link_directories(${PROJECT_NAME} PRIVATE ${LIBHEIF_LIBRARY_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${LIBHEIF_LIBRARIES})
