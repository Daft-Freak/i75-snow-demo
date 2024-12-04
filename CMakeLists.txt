cmake_minimum_required(VERSION 3.12)

# Pull in PICO SDK (must be before project)
include(pico_sdk_import.cmake)
include(pimoroni_pico_import.cmake)

project(snow-demo)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

option(MATRIX_2X1 "2x1 Matrix (64x32)" OFF)
option(MATRIX_2X2 "2x2 Matrix (64x64)" OFF)

option(WIFI_SSID "WiFi SSID" "")
option(WIFI_PASSWORD "WiFi password" "")

# Initialize the SDK
pico_sdk_init()

include(libraries/interstate75/interstate75)

set(OUTPUT_NAME interstate75_snow)
add_executable(${OUTPUT_NAME} snow.cpp)

# for lwipopts.h
target_include_directories(${OUTPUT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# enable usb output
pico_enable_stdio_usb(${OUTPUT_NAME} 1)

pico_add_extra_outputs(${OUTPUT_NAME})

target_link_libraries(${OUTPUT_NAME}
    pico_stdlib
    pico_multicore
    pico_rand
    hub75
)

if(MATRIX_2X1)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE -DMATRIX_2X1)
endif()

if(MATRIX_2X2)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE -DMATRIX_2X2)
endif()

if(WIFI_SSID AND WIFI_PASSWORD)
    # assume pico w
    target_link_libraries(${OUTPUT_NAME} pico_cyw43_arch_lwip_threadsafe_background)
    target_compile_definitions(${OUTPUT_NAME} PRIVATE
        WIFI_SSID="${WIFI_SSID}"
        WIFI_PASSWORD="${WIFI_PASSWORD}"
        WIFI_ENABLED
    )
endif()