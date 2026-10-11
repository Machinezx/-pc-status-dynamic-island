cmake_minimum_required(VERSION 3.20)
project(ResourceIsland LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(ResourceIsland WIN32
  src/main.cpp
  src/island.cpp
  src/render.cpp
  src/sampler.cpp
  src/nvml_dyn.cpp
  src/config.cpp
  src/fps.cpp
  src/avatar_data.cpp
)

target_compile_definitions(ResourceIsland PRIVATE
  UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NOMINMAX)

if (MSVC)
  target_compile_options(ResourceIsland PRIVATE /utf-8 /W4 /permissive- /EHsc)
  target_link_options(ResourceIsland PRIVATE /OPT:REF /OPT:ICF)
endif()

target_link_libraries(ResourceIsland PRIVATE user32 gdi32 shell32 advapi32 pdh)
