cmake_minimum_required(VERSION 3.22)
project(guest_inventory_mod LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(guest_inventory_mod SHARED src/inventory.cpp)
target_include_directories(guest_inventory_mod PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/modtopia/sdk/include")
set_target_properties(guest_inventory_mod PROPERTIES PREFIX "" OUTPUT_NAME "mod")
if(APPLE)
    set_target_properties(guest_inventory_mod PROPERTIES SUFFIX ".dylib")
endif()
