cmake_minimum_required(VERSION 3.28)
project(binance_tui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# For MacOS.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

include(FetchContent)

# Include FTXUI.
FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v6.1.9
)
FetchContent_MakeAvailable(ftxui)

find_package(Boost REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)

add_library(binance_tui INTERFACE)
target_include_directories(binance_tui INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(binance_tui INTERFACE
  Threads::Threads
  Boost::headers
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  nlohmann_json::nlohmann_json
  yaml-cpp::yaml-cpp
  ftxui::screen
  ftxui::dom
  ftxui::component
  spdlog::spdlog
  # spdlog::spdlog_header_only
)

add_executable(main src/main.cpp)
target_link_libraries(main binance_tui pthread)
target_compile_options(main PRIVATE -Wall -Wextra -Wpedantic)
