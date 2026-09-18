cmake_minimum_required(VERSION 3.14)
project(chusanwide CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 32-bit process, so the DLL must be 32-bit too
#   MSVC:  cmake -B build -A Win32
#   MinGW: use the toolchain file in cmake/
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Must be built as 32-bit. MSVC: cmake -B build -A Win32")
endif()

include(FetchContent)
FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(chusanwide SHARED src/dllmain.cpp)
target_link_libraries(chusanwide PRIVATE minhook d3d9)
set_target_properties(chusanwide PROPERTIES PREFIX "" OUTPUT_NAME "chusanwide")

# debug dll -DCHUSANWIDE_DEBUG_DLL=ON
option(CHUSANWIDE_DEBUG_DLL "Also build the diagnostic DLL" OFF)
if(CHUSANWIDE_DEBUG_DLL)
  add_library(chusanwide_debug SHARED debug/dllmain_debug.cpp)
  target_link_libraries(chusanwide_debug PRIVATE minhook d3d9)
  set_target_properties(chusanwide_debug PROPERTIES PREFIX "" OUTPUT_NAME "chusanwide_debug")
endif()

if(MSVC)
  # Static CRT, so u need no Visual C++ redistributable
  foreach(t chusanwide)
    target_compile_options(${t} PRIVATE $<$<CONFIG:Release>:/MT> $<$<CONFIG:Debug>:/MTd>)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
  endforeach()
  set_property(TARGET chusanwide PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
  # Link libgcc/libstdc++ statically for the same reason.
  target_link_options(chusanwide PRIVATE -static -static-libgcc -static-libstdc++)
  target_compile_definitions(chusanwide PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(CHUSANWIDE_DEBUG_DLL)
    target_link_options(chusanwide_debug PRIVATE -static -static-libgcc -static-libstdc++)
    target_compile_definitions(chusanwide_debug PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  endif()
endif()
