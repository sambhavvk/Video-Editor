# SPDX-License-Identifier: MPL-2.0

find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
  set(VIDEO_EDITOR_SQLITE_TARGET SQLite3::SQLite3)
else()
  set(VIDEO_EDITOR_SQLITE_TARGET SQLite::SQLite3)
endif()

add_library(video_editor_project_store STATIC
  src/project_store.cpp
  src/sqlite_connection.cpp
)
add_library(video_editor::project_store ALIAS video_editor_project_store)

target_compile_features(video_editor_project_store PUBLIC cxx_std_20)
target_include_directories(video_editor_project_store
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(video_editor_project_store
  PUBLIC
    video_editor::project_options
    ${VIDEO_EDITOR_SQLITE_TARGET}
  PRIVATE
    video_editor::project_warnings
)
