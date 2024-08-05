cmake_minimum_required(VERSION 3.14)
project(3dslink)

add_definitions(-DPACKAGE_STRING="") 

add_executable(3dslink host/src/main.c)
target_link_libraries(3dslink -lz)