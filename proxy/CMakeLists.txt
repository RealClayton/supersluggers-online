cmake_minimum_required(VERSION 3.15)
project(SluggersProxy CXX)

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
# This assumes hidapi is installed on the host system.
# Windows: FetchContent or local dll link.
# macOS: brew install hidapi (find_package(hidapi))
# Linux: apt-get install libhidapi-dev
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI hidapi-libusb)

if(HIDAPI_FOUND)
    target_include_directories(SluggersProxy PRIVATE ${HIDAPI_INCLUDE_DIRS})
    target_link_libraries(SluggersProxy PRIVATE ${HIDAPI_LIBRARIES})
    target_compile_definitions(SluggersProxy PRIVATE HAS_HIDAPI=1)
    message(STATUS "hidapi found and linked successfully.")
else()
    message(WARNING "hidapi not found on host. The proxy will build with mocked device streams.")
endif()
