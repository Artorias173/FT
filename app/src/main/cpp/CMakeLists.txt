cmake_minimum_required(VERSION 3.22.1)
project(ftvita LANGUAGES CXX)

# Le cœur portable vit à la racine du dépôt, dans core/.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../core ${CMAKE_BINARY_DIR}/core)

add_library(ftvita SHARED jni_bridge.cpp)
target_link_libraries(ftvita PRIVATE vita_core android log)
