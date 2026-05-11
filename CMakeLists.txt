# Top-level CMakeLists.txt
# Every ESP-IDF project needs this at the root.
# It just tells CMake where the project lives and includes the IDF framework.

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(plantagochi)