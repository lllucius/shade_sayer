# Color Detection Device for Visually Impaired Users
# ESP-IDF Project Main CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF project setup
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)

# Set project name
project(color_detector)

