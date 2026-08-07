# SPDX-License-Identifier: MPL-2.0

if(TARGET video_editor_edit_model AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/edit_model/CMakeLists.txt")
  add_subdirectory(edit_model)
endif()

if(TARGET video_editor_caption_service AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/caption_service/CMakeLists.txt")
  add_subdirectory(caption_service)
endif()

if(TARGET video_editor_project_codec AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/project_codec/CMakeLists.txt")
  add_subdirectory(project_codec)
endif()

if(TARGET video_editor_project_store AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/project_store/CMakeLists.txt")
  add_subdirectory(project_store)
endif()

if(TARGET video_editor_media_codec)
  add_subdirectory(media_codec)
endif()

if(TARGET video_editor_asset_service)
  add_subdirectory(asset_service)
endif()

if(TARGET video_editor_proxy_service AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proxy_service/CMakeLists.txt")
  add_subdirectory(proxy_service)
endif()

if(TARGET video_editor_render_engine AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/render_engine/CMakeLists.txt")
  add_subdirectory(render_engine)
endif()

if(TARGET video_editor_export_service AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/export_service/CMakeLists.txt")
  add_subdirectory(export_service)
endif()

if(TARGET video_editor_playback AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/playback/CMakeLists.txt")
  add_subdirectory(playback)
endif()

if(TARGET video_editor_audio_engine AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/audio_engine/CMakeLists.txt")
  add_subdirectory(audio_engine)
endif()

if(TARGET video_editor_job_service AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/job_service/CMakeLists.txt")
  add_subdirectory(job_service)
endif()

if(TARGET video_editor_desktop_ui AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/desktop_ui/CMakeLists.txt")
  add_subdirectory(desktop_ui)
endif()

if(TARGET video_editor_app_controller AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/CMakeLists.txt")
  add_subdirectory(app)
endif()
