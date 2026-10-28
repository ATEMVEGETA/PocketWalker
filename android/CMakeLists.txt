cmake_minimum_required(VERSION 3.25)

project(pocketwalker_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network)

set(POCKETWALKER_RTC_DEBUG_LOG OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core core)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/json nlohmann)

qt_add_executable(pocketwalker_android MANUAL_FINALIZATION
    src/main.cpp
    src/android_runtime_bridge.cpp
    src/android_runtime_bridge.h
    src/android_display_widget.cpp
    src/android_display_widget.h
    src/android_window.cpp
    src/android_window.h
    ../desktop/src/qt/audio/qt_audio_system.cpp
    ../desktop/src/qt/audio/qt_audio_system.h
    ../desktop/src/qt/emulator/emulator_context.cpp
    ../desktop/src/qt/emulator/emulator_context.h
    ../desktop/src/qt/network/qt_network_system.cpp
    ../desktop/src/qt/network/qt_network_system.h
    ../desktop/src/qt/settings/app_settings.cpp
    ../desktop/src/qt/settings/app_settings.h
)

target_include_directories(pocketwalker_android PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(pocketwalker_android PRIVATE
    pocketwalker_core
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
    nlohmann_json::nlohmann_json
)

set_target_properties(pocketwalker_android PROPERTIES
    OUTPUT_NAME "pocketwalker_android"
    QT_ANDROID_DEPLOYMENT_TYPE Debug
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/package"
    QT_ANDROID_PACKAGE_NAME "org.atemvegeta.pocketwalker"
    QT_ANDROID_MIN_SDK_VERSION 29
    QT_ANDROID_TARGET_SDK_VERSION 36
    QT_ANDROID_VERSION_CODE 60
    QT_ANDROID_VERSION_NAME "0.52.1"
)

qt_finalize_executable(pocketwalker_android)
