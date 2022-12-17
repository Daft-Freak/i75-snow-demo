cmake_minimum_required(VERSION 3.12)

# Pull in PICO SDK (must be before project)
include(pico_sdk_import.cmake)
include(pimoroni_pico_import.cmake)

project(snow-demo)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(MATRIX_2X1 "2x1 Matrix (64x32)" OFF)
option(MATRIX_2X2 "2x2 Matrix (64x64)" OFF)

# Initialize the SDK
pico_sdk_init()

include(drivers/hub75_legacy/hub75)

set(OUTPUT_NAME interstate75_snow)
add_executable(${OUTPUT_NAME} snow.cpp)

# enable usb output
pico_enable_stdio_usb(${OUTPUT_NAME} 1)

pico_add_extra_outputs(${OUTPUT_NAME})

target_link_libraries(${OUTPUT_NAME}
    pico_stdlib
    pico_multicore
    hub75_legacy
)


if(MATRIX_2X1)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE -DMATRIX_2X1)
endif()

if(MATRIX_2X2)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE -DMATRIX_2X2)
endif()