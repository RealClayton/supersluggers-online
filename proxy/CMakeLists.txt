cmake_minimum_required(VERSION 3.15)
project(SluggersProxy C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/main.cpp
)

# Executable output
add_executable(SluggersProxy ${SOURCES})

# Cross-platform networking dependencies
if(WIN32)
    target_link_libraries(SluggersProxy PRIVATE ws2_32)
else()
    target_link_libraries(SluggersProxy PRIVATE pthread)
endif()

# HIDAPI Dependency (Include and Link)
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(HIDAPI hidapi-libusb)
endif()

if(HIDAPI_FOUND)
    target_include_directories(SluggersProxy PRIVATE ${HIDAPI_INCLUDE_DIRS})
    target_link_libraries(SluggersProxy PRIVATE ${HIDAPI_LIBRARIES})
    target_compile_definitions(SluggersProxy PRIVATE HAS_HIDAPI=1)
    message(STATUS "hidapi found and linked successfully.")
else()
    # Check if we can use the bundled hidapi in dolphin-src
    set(BUNDLED_HIDAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../dolphin-src/Externals/hidapi/hidapi-src")
    if(EXISTS "${BUNDLED_HIDAPI_DIR}")
        message(STATUS "Using bundled hidapi from ${BUNDLED_HIDAPI_DIR}")
        target_include_directories(SluggersProxy PRIVATE "${BUNDLED_HIDAPI_DIR}/hidapi" "${BUNDLED_HIDAPI_DIR}")
        
        if(WIN32)
            target_sources(SluggersProxy PRIVATE 
                "${BUNDLED_HIDAPI_DIR}/windows/hid.c"
            )
            target_link_libraries(SluggersProxy PRIVATE setupapi)
            target_compile_definitions(SluggersProxy PRIVATE HAS_HIDAPI=1)
            message(STATUS "Bundled Windows hidapi compiled and linked successfully.")
        elseif(APPLE)
            target_sources(SluggersProxy PRIVATE 
                "${BUNDLED_HIDAPI_DIR}/mac/hid.c"
            )
            find_library(IOKIT IOKit REQUIRED)
            find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
            target_link_libraries(SluggersProxy PRIVATE ${IOKIT} ${CORE_FOUNDATION})
            target_compile_definitions(SluggersProxy PRIVATE HAS_HIDAPI=1)
            message(STATUS "Bundled macOS hidapi compiled and linked successfully.")
        else()
            target_sources(SluggersProxy PRIVATE 
                "${BUNDLED_HIDAPI_DIR}/linux/hid.c"
            )
            target_link_libraries(SluggersProxy PRIVATE udev)
            target_compile_definitions(SluggersProxy PRIVATE HAS_HIDAPI=1)
            message(STATUS "Bundled Linux hidapi compiled and linked successfully.")
        endif()
    else()
        message(WARNING "hidapi not found on host and bundled hidapi source not found. Building proxy with mocked device streams.")
    endif()
endif()
