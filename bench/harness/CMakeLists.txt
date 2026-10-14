# Billet benchmark harness. Builds against an installed Billet/OCCT, for example:
#   cmake -S bench/harness -B build-bench -G "Visual Studio 17 2022" -A x64 -DOpenCASCADE_DIR=C:/billet-install/cmake
#   cmake --build build-bench --config Release

cmake_minimum_required(VERSION 3.20)
project(BilletBench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCASCADE REQUIRED CONFIG)

add_executable(billet-bench
  src/main.cpp
  src/operations.cpp
  src/fingerprint.cpp
  src/modeling.cpp
  src/parts.cpp
  src/probe.cpp)

target_include_directories(billet-bench SYSTEM PRIVATE "${OpenCASCADE_INCLUDE_DIR}")
target_link_libraries(billet-bench PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep
  TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKShHealing TKFillet TKOffset TKMesh
  TKCDF TKLCAF TKCAF TKDE TKXSBase TKDESTEP TKXCAF)

if (WIN32)
  target_link_libraries(billet-bench PRIVATE psapi)
endif()
if (MSVC)
  target_compile_options(billet-bench PRIVATE /W4 /permissive- /utf-8)
  # A PDB in Release, for the profiler (bench/runner/billet_profile.py). /Zi does not change the code;
  # /DEBUG turns off /OPT:REF and /OPT:ICF by default, so they are restated.
  target_compile_options(billet-bench PRIVATE $<$<CONFIG:Release>:/Zi>)
  target_link_options(billet-bench PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()
