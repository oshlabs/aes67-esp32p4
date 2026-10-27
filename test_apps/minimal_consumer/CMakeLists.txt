# Minimal consumer of the aes67 component: proves the component builds and
# links on its own (web UI included), without the example application.
#   cd test_apps/minimal_consumer && idf.py set-target esp32p4 && idf.py build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(minimal_consumer)
