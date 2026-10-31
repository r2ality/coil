cmake_minimum_required(VERSION 3.16)
project(SoundFetch VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SoundFetch only uses FLTK's core widgets. Do not make optional FLTK
# components (GL, Forms, Images, or Fluid) build-time requirements.
set(FLTK_SKIP_OPENGL TRUE)
set(FLTK_SKIP_FORMS TRUE)
set(FLTK_SKIP_IMAGES TRUE)
set(FLTK_SKIP_FLUID TRUE)
find_package(FLTK REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Threads REQUIRED)
find_program(GO_EXECUTABLE go REQUIRED)

add_executable(soundfetch frontend/main.cpp)
target_include_directories(soundfetch PRIVATE ${FLTK_INCLUDE_DIR} ${FLTK_INCLUDE_DIRS})
target_link_libraries(soundfetch PRIVATE ${FLTK_LIBRARIES} nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(soundfetch PRIVATE -Wall -Wextra -Wpedantic)

set(BACKEND_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/soundfetch-backend")
# Nix sandbox builds do not provide a writable HOME. Keep all Go caches and
# module downloads under the writable CMake build directory instead.
set(GO_CACHE_DIR "${CMAKE_CURRENT_BINARY_DIR}/go-cache")
set(GO_PATH_DIR "${CMAKE_CURRENT_BINARY_DIR}/go-path")
set(GO_MOD_CACHE_DIR "${GO_PATH_DIR}/pkg/mod")
add_custom_command(
    OUTPUT "${BACKEND_OUTPUT}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "GOCACHE=${GO_CACHE_DIR}"
        "GOPATH=${GO_PATH_DIR}"
        "GOMODCACHE=${GO_MOD_CACHE_DIR}"
        "GOTELEMETRY=off"
        "GOTOOLCHAIN=local"
        "${GO_EXECUTABLE}" build -trimpath "-ldflags=-s -w" -o "${BACKEND_OUTPUT}" ./backend
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS backend/main.go go.mod
    COMMENT "Building the Go backend"
    VERBATIM
)
# Use a build target name distinct from the output filename. Sharing the same
# name made GNU Make generate a circular target dependency.
add_custom_target(soundfetch_backend_build ALL DEPENDS "${BACKEND_OUTPUT}")

install(TARGETS soundfetch RUNTIME DESTINATION bin)
install(PROGRAMS "${BACKEND_OUTPUT}" DESTINATION bin)
install(FILES soundfetch.desktop DESTINATION share/applications)
install(FILES README.md LICENSE DESTINATION share/doc/soundfetch)
