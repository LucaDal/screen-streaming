cmake_minimum_required(VERSION 3.21)

project(StreamingApp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Network WebSockets)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.20 gstreamer-app-1.0>=1.20 gstreamer-video-1.0>=1.20
    gstreamer-webrtc-1.0>=1.20 gstreamer-sdp-1.0>=1.20)
qt_standard_project_setup()

add_library(video_pipeline src/VideoPipeline.cpp src/VideoPipeline.h)
target_include_directories(video_pipeline PUBLIC src)
target_link_libraries(video_pipeline PUBLIC Qt6::Multimedia PkgConfig::GSTREAMER PRIVATE Threads::Threads)

add_library(streaming_network src/network/WebRtcPeer.cpp src/network/WebRtcPeer.h
    src/network/RoomClient.cpp src/network/RoomClient.h
    src/network/SignalingServer.cpp src/network/SignalingServer.h)
target_compile_definitions(streaming_network PRIVATE GST_USE_UNSTABLE_API)
target_include_directories(streaming_network PUBLIC src/network)
target_link_libraries(streaming_network PUBLIC video_pipeline Qt6::Network Qt6::WebSockets)
qt_add_executable(signaling_server src/network/server_main.cpp)
target_link_libraries(signaling_server PRIVATE streaming_network)

qt_add_executable(streaming_app
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/ScreenCaptureController.cpp
    src/ScreenCaptureController.h
)

target_link_libraries(streaming_app PRIVATE
    Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets streaming_network
)

if(MSVC)
    target_compile_options(streaming_app PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(video_pipeline PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(streaming_network PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(signaling_server PRIVATE /utf-8)
else()
    target_compile_options(streaming_app PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(video_pipeline PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(streaming_network PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Only Wayland needs explicit portal lifetime management. Windows keeps the
# native QScreenCapture path without a D-Bus dependency.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.5 REQUIRED COMPONENTS DBus)
    add_library(portal_session src/PortalSessionGuard.cpp src/PortalSessionGuard.h)
    target_include_directories(portal_session PUBLIC src)
    target_link_libraries(portal_session PUBLIC Qt6::DBus)
    target_compile_definitions(portal_session INTERFACE STREAMING_PORTAL_GUARD)
    target_link_libraries(streaming_app PRIVATE portal_session)
endif()

set_target_properties(streaming_app PROPERTIES WIN32_EXECUTABLE TRUE)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(webrtc_test tests/WebRtcTest.cpp)
    target_link_libraries(webrtc_test PRIVATE streaming_network)
    qt_add_executable(room_test tests/RoomTest.cpp)
    target_link_libraries(room_test PRIVATE streaming_network)
    add_test(NAME room_invitation_and_handoff COMMAND room_test)
    qt_add_executable(mainwindow_test tests/MainWindowTest.cpp src/MainWindow.cpp src/MainWindow.h
        src/ScreenCaptureController.cpp src/ScreenCaptureController.h)
    target_include_directories(mainwindow_test PRIVATE src)
    target_link_libraries(mainwindow_test PRIVATE Qt6::Widgets Qt6::MultimediaWidgets streaming_network)
    if(MSVC)
        target_compile_options(mainwindow_test PRIVATE /utf-8)
        target_compile_options(room_test PRIVATE /utf-8)
        target_compile_options(webrtc_test PRIVATE /utf-8)
    endif()
    if(TARGET portal_session)
        target_link_libraries(mainwindow_test PRIVATE portal_session)
        qt_add_executable(portal_session_test tests/PortalSessionTest.cpp)
        target_link_libraries(portal_session_test PRIVATE portal_session)
        find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
        add_test(NAME portal_session_cleanup COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:portal_session_test>)
        set_tests_properties(portal_session_cleanup PROPERTIES TIMEOUT 15)
    endif()
    add_test(NAME fullscreen_native_surface COMMAND mainwindow_test)
    add_test(NAME saved_server_key COMMAND mainwindow_test --settings)
    set_tests_properties(fullscreen_native_surface saved_server_key PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME webrtc_loopback COMMAND webrtc_test)
    add_test(NAME webrtc_audio_loopback COMMAND webrtc_test --audio)
    set_tests_properties(webrtc_loopback webrtc_audio_loopback room_invitation_and_handoff PROPERTIES TIMEOUT 45 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;GST_PLUGIN_PATH_1_0=${CMAKE_CURRENT_SOURCE_DIR}/.local/gstreamer-1.0"
        SKIP_RETURN_CODE 77)
    qt_add_executable(video_pipeline_test tests/VideoPipelineTest.cpp)
    target_link_libraries(video_pipeline_test PRIVATE video_pipeline)
    add_test(NAME video_pipeline COMMAND video_pipeline_test)
    set_tests_properties(video_pipeline PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_test(NAME missing_video_plugins COMMAND video_pipeline_test --missing-plugins)
    set_tests_properties(missing_video_plugins PROPERTIES TIMEOUT 10 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;GST_PLUGIN_PATH_1_0=;GST_PLUGIN_SYSTEM_PATH_1_0=;GST_REGISTRY_1_0=${CMAKE_CURRENT_BINARY_DIR}/empty-plugin-registry.bin")
endif()
