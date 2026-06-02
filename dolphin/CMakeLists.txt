cmake_minimum_required(VERSION 3.15)
project(DolphinTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    input_injection.cpp
    input_injection.h
)

# Compile test targets
add_executable(dolphin_test dolphin_test.cpp ${SOURCES})
add_executable(integration_test integration_test.cpp ${SOURCES})
add_executable(stress_test stress_test.cpp ${SOURCES})

# Cross-platform networking dependencies
if(WIN32)
    target_link_libraries(dolphin_test PRIVATE ws2_32)
    target_link_libraries(integration_test PRIVATE ws2_32)
    target_link_libraries(stress_test PRIVATE ws2_32)
else()
    target_link_libraries(dolphin_test PRIVATE pthread)
    target_link_libraries(integration_test PRIVATE pthread)
    target_link_libraries(stress_test PRIVATE pthread)
endif()
