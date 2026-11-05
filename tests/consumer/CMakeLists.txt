# A project that pulls libpg in as a subproject and sets no C++ standard of its
# own: target pg must carry everything its headers need.
cmake_minimum_required(VERSION 3.25)
project(libpg_consumer LANGUAGES CXX)
add_subdirectory(${LIBPG_SOURCE_DIR} libpg)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE pg)
