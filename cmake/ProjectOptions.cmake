# SPDX-License-Identifier: MPL-2.0

include_guard(GLOBAL)

option(VIDEO_EDITOR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VIDEO_EDITOR_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

function(video_editor_configure_project)
  set(CMAKE_CXX_STANDARD 20 PARENT_SCOPE)
  set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
  set(CMAKE_CXX_EXTENSIONS OFF PARENT_SCOPE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON PARENT_SCOPE)

  add_library(video_editor_project_options INTERFACE)
  add_library(video_editor::project_options ALIAS video_editor_project_options)
  target_compile_features(video_editor_project_options INTERFACE cxx_std_20)

  add_library(video_editor_project_warnings INTERFACE)
  add_library(video_editor::project_warnings ALIAS video_editor_project_warnings)

  if(MSVC)
    target_compile_options(video_editor_project_warnings INTERFACE /W4 /permissive- /EHsc)
    if(VIDEO_EDITOR_WARNINGS_AS_ERRORS)
      target_compile_options(video_editor_project_warnings INTERFACE /WX)
    endif()
  else()
    target_compile_options(
      video_editor_project_warnings
      INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
                -Wshadow -Wnon-virtual-dtor -Wold-style-cast)
    if(VIDEO_EDITOR_WARNINGS_AS_ERRORS)
      target_compile_options(video_editor_project_warnings INTERFACE -Werror)
    endif()
  endif()

  if(VIDEO_EDITOR_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(video_editor_project_options INTERFACE
                           -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(video_editor_project_options INTERFACE
                        -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endfunction()
