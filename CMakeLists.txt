cmake_minimum_required(VERSION 3.20)
project(ce_debug C CXX)

include(Grimace)

add_subdirectory(agent)
if(GRIMACE_PLATFORM STREQUAL "brain")
    add_subdirectory(usbfn)
endif()
