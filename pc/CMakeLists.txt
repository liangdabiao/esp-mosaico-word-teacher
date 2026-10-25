cmake_minimum_required(VERSION 3.20)
project(wt_backend LANGUAGES C)
if(CMAKE_C_COMPILER MATCHES "python-zig")
    # Zig is the installed native compiler here; it also supplies its archiver.
    set(CMAKE_C_ARCHIVE_CREATE "\"${CMAKE_C_COMPILER}\" ar qc <TARGET> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH "")
endif()

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
gsp_add_backend(wt_backend
    SOURCES ../main/wt_ui.c ../main/wt_model.c ../main/wt_controls.c
        ../main/wt_content.c ../main/wt_manifest.c
        ../main/wt_progress.c ../main/wt_quiz.c platform_pc.c
    INCLUDE_DIRECTORIES ../main
    SCENES ../ui/main.json)
# 模拟器平台层从磁盘加载真实词库 blob（只读，不联网）。
target_compile_definitions(wt_backend PRIVATE
    WT_CONTENT_BIN="${CMAKE_CURRENT_LIST_DIR}/../assets/content/wt_content.bin")

set(WT_MODEL_SOURCES ../main/wt_model.c ../main/wt_controls.c
    ../main/wt_content.c ../main/wt_manifest.c ../main/wt_progress.c ../main/wt_quiz.c)
add_executable(wt_model_test wt_model_test.c ${WT_MODEL_SOURCES})
target_include_directories(wt_model_test PRIVATE ../main)
add_executable(controls_test controls_test.c ../main/wt_controls.c)
target_include_directories(controls_test PRIVATE ../main)
add_executable(speech_conversation_test speech_conversation_test.c ../main/wt_speech.c ../main/wt_conversation.c)
target_include_directories(speech_conversation_test PRIVATE stubs ../main)
add_executable(provider_protocol_test provider_protocol_test.c ../main/wt_provider.c
    ../main/wt_config.c ../main/wt_controls.c ../managed_components/espressif__cjson/cJSON/cJSON.c)
target_include_directories(provider_protocol_test PRIVATE stubs ../main ../managed_components/espressif__cjson/cJSON)
add_executable(test_wt_content test_wt_content.c ../main/wt_content.c ../main/wt_manifest.c)
target_include_directories(test_wt_content PRIVATE ../main)
add_executable(test_wt_manifest test_wt_manifest.c ../main/wt_manifest.c)
target_include_directories(test_wt_manifest PRIVATE ../main)
add_executable(test_wt_progress test_wt_progress.c ../main/wt_progress.c ../main/wt_manifest.c)
target_include_directories(test_wt_progress PRIVATE ../main)
add_executable(test_wt_quiz test_wt_quiz.c ../main/wt_quiz.c ../main/wt_content.c ../main/wt_manifest.c)
target_include_directories(test_wt_quiz PRIVATE ../main)
if(MSVC)
    target_compile_options(controls_test PRIVATE /UNDEBUG)
    target_compile_options(speech_conversation_test PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(wt_model_test PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(provider_protocol_test PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(test_wt_content PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(test_wt_manifest PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(test_wt_progress PRIVATE /utf-8 /UNDEBUG)
    target_compile_options(test_wt_quiz PRIVATE /utf-8 /UNDEBUG)
else()
    target_compile_options(controls_test PRIVATE -UNDEBUG)
    target_compile_options(speech_conversation_test PRIVATE -UNDEBUG)
    target_compile_options(wt_model_test PRIVATE -UNDEBUG)
    target_compile_options(provider_protocol_test PRIVATE -UNDEBUG)
    target_compile_options(test_wt_content PRIVATE -UNDEBUG)
    target_compile_options(test_wt_manifest PRIVATE -UNDEBUG)
    target_compile_options(test_wt_progress PRIVATE -UNDEBUG)
    target_compile_options(test_wt_quiz PRIVATE -UNDEBUG)
endif()
