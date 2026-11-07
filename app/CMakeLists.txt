cmake_minimum_required(VERSION 3.19)
project(PrintMonitor VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

qt_add_executable(print-monitor
    src/main.cpp
    src/ApiClient.h
    src/ApiClient.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

target_link_libraries(print-monitor PRIVATE Qt6::Widgets Qt6::Network)
target_compile_options(print-monitor PRIVATE -Wall -Wextra)
