# Host-contract tests for the VR renderer, input and camera code, included by
# CMakeLists.txt next to cmake/PlatformTests.cmake. Unlike those they need the
# product configuration's vendored toml11 and OpenXR loader target, so the
# dependency-free MKW_PLATFORM_TESTS_ONLY build does not include them.
get_filename_component(MKW_VR_TEST_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_executable(mkw_frame_interpolation_pacing_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/frame_interpolation_pacing_tests.cpp")
target_include_directories(mkw_frame_interpolation_pacing_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_link_libraries(mkw_frame_interpolation_pacing_tests PRIVATE mkw::toml11)
target_compile_features(mkw_frame_interpolation_pacing_tests PRIVATE cxx_std_20)
add_test(NAME mkw_frame_interpolation_pacing_tests COMMAND mkw_frame_interpolation_pacing_tests)
# [vr] settings the headset renderer reads: single_pass_eyes and the Quest's foveation.
add_executable(mkw_vr_config_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_config_tests.cpp")
target_include_directories(mkw_vr_config_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_link_libraries(mkw_vr_config_tests PRIVATE mkw::toml11)
target_compile_features(mkw_vr_config_tests PRIVATE cxx_std_20)
add_test(NAME mkw_vr_config_tests COMMAND mkw_vr_config_tests)

# The first-person VR camera's transform is deliberately header-only and free of
# guest access so it can be checked here, without a headset or a running game.
add_executable(mkw_vr_first_person_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_first_person_tests.cpp")
target_include_directories(mkw_vr_first_person_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_first_person_tests PRIVATE cxx_std_17)
add_test(NAME mkw_vr_first_person_tests COMMAND mkw_vr_first_person_tests)

# The first-person cockpit (seated eye, wheel and handlebar geometry, level seat,
# native wheel vertices) and hand steering (grab, turn, hand-off to the game),
# ported from heurazy's mario-kart-wii-VR-port, tracked hands (grasp, bare-hand
# buttons, flick), throwing the held item, the character body IK and the race
# camera modes (from heurazy's Wiicompiled_VR-PLUS). All header-only.
foreach(test_name mkw_steering_wheel_tests mkw_vr_cockpit_tests mkw_vr_bullet_bill_tests mkw_vr_hand_steering_tests mkw_vr_camera_toggle_tests
                  mkw_vr_hand_tracking_tests mkw_vr_culling_tests mkw_vr_item_throw_tests mkw_vr_body_ik_tests
                  mkw_vr_camera_mode_tests)
    string(REGEX REPLACE "^mkw_" "" test_source "${test_name}")
    add_executable(${test_name} "${MKW_VR_TEST_RUNTIME_DIR}/tests/${test_source}.cpp")
    target_include_directories(${test_name} PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
    target_compile_features(${test_name} PRIVATE cxx_std_17)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
# The body IK ships in mkw_vr_first_person.cpp, compiled with the runtime's
# -ffast-math but -fno-finite-math-only (CMakeLists.txt), so that its NaN
# guards on computed values survive; they are tested under the same flags.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mkw_vr_body_ik_tests PRIVATE -ffast-math -fno-finite-math-only)
endif()

# The USB wheel's calibration and GameCube pad mapping (physical_wheel.h),
# ported from heurazy's mario-kart-wii-VR-port. Header-only; needs dolphin/pad.h.
add_executable(mkw_physical_wheel_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/physical_wheel_tests.cpp")
target_include_directories(mkw_physical_wheel_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include"
                                                           "${MKW_VR_TEST_RUNTIME_DIR}/../aurora-main/include")
target_compile_definitions(mkw_physical_wheel_tests PRIVATE TARGET_PC)
target_compile_features(mkw_physical_wheel_tests PRIVATE cxx_std_17)
add_test(NAME mkw_physical_wheel_tests COMMAND mkw_physical_wheel_tests)

# The VR controllers' Wii Remote presentation (accelerometer frame, pointer
# raycast, debounce, button profile) is header-only for the same reason.
add_executable(mkw_vr_wii_remote_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_wii_remote_tests.cpp")
target_include_directories(mkw_vr_wii_remote_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_wii_remote_tests PRIVATE cxx_std_17)
add_test(NAME mkw_vr_wii_remote_tests COMMAND mkw_vr_wii_remote_tests)

# The in-headset settings panel's controller chord, release latch, selection,
# scrolling and canvas mapping, plus the thread bridge they publish through.
add_executable(mkw_vr_settings_panel_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_settings_panel_tests.cpp"
                                           "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_settings_panel.cpp")
target_include_directories(mkw_vr_settings_panel_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_settings_panel_tests PRIVATE cxx_std_17)
set_target_properties(mkw_vr_settings_panel_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_vr_settings_panel_tests COMMAND mkw_vr_settings_panel_tests)

# Resolve the real local-kart pointer walk against synthetic offline/online rosters.
add_executable(mkw_vr_player_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_player_tests.cpp")
target_include_directories(mkw_vr_player_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_player_tests PRIVATE cxx_std_17)
add_test(NAME mkw_vr_player_tests COMMAND mkw_vr_player_tests)

add_executable(mkw_vr_item_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_item_tests.cpp")
target_include_directories(mkw_vr_item_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_item_tests PRIVATE cxx_std_17)
add_test(NAME mkw_vr_item_tests COMMAND mkw_vr_item_tests)

add_executable(mkw_vr_policy_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/vr_policy_tests.cpp"
                                   "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/mkw_vr_policy.cpp")
target_include_directories(mkw_vr_policy_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_vr_policy_tests PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mkw_vr_policy_tests PRIVATE -ffast-math)
endif()
add_test(NAME mkw_vr_policy_tests COMMAND mkw_vr_policy_tests)

# F10 > Diagnostics: the OpenXR frame-diagnostics collector is free of OpenXR
# and guest state, so its window accounting runs on a synthetic clock here.
add_executable(mkw_openxr_diagnostics_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/openxr_diagnostics_tests.cpp"
                                            "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_diagnostics.cpp")
target_include_directories(mkw_openxr_diagnostics_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_link_libraries(mkw_openxr_diagnostics_tests PRIVATE mkw::toml11)
target_compile_features(mkw_openxr_diagnostics_tests PRIVATE cxx_std_20)
set_target_properties(mkw_openxr_diagnostics_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_openxr_diagnostics_tests COMMAND mkw_openxr_diagnostics_tests)

# F10 > Diagnostics > Export Logs, against temporary folders.
add_executable(mkw_log_export_tests "${MKW_VR_TEST_RUNTIME_DIR}/tests/log_export_tests.cpp"
                                    "${MKW_VR_TEST_RUNTIME_DIR}/src/log_export.cpp")
target_include_directories(mkw_log_export_tests PRIVATE "${MKW_VR_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_log_export_tests PRIVATE cxx_std_20)
set_target_properties(mkw_log_export_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_log_export_tests COMMAND mkw_log_export_tests)

if(MKW_ENABLE_OPENXR AND MKW_PLATFORM_WINDOWS)
    add_executable(mkw_openxr_replay_tests
        "${MKW_VR_TEST_RUNTIME_DIR}/tests/openxr_d3d12_replay_tests.cpp"
        "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_d3d12.cpp"
        "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_diagnostics.cpp")
    target_include_directories(mkw_openxr_replay_tests PRIVATE
        "${MKW_VR_TEST_RUNTIME_DIR}/include"
        "${MKW_VR_TEST_RUNTIME_DIR}/../aurora-main/include"
        "$<TARGET_PROPERTY:${MKW_OPENXR_TARGET},INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(mkw_openxr_replay_tests PRIVATE MKW_ENABLE_OPENXR=1)
    target_compile_features(mkw_openxr_replay_tests PRIVATE cxx_std_17)
    set_target_properties(mkw_openxr_replay_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_openxr_replay_tests COMMAND mkw_openxr_replay_tests)
    add_executable(mkw_openxr_vulkan_replay_tests
        "${MKW_VR_TEST_RUNTIME_DIR}/tests/openxr_d3d12_replay_tests.cpp"
        "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_vulkan_win32.cpp"
        "${MKW_VR_TEST_RUNTIME_DIR}/src/vr/openxr_diagnostics.cpp")
    target_include_directories(mkw_openxr_vulkan_replay_tests PRIVATE
        "${MKW_VR_TEST_RUNTIME_DIR}/include"
        "${MKW_VR_TEST_RUNTIME_DIR}/../aurora-main/include"
        "$<TARGET_PROPERTY:${MKW_OPENXR_TARGET},INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(mkw_openxr_vulkan_replay_tests PRIVATE MKW_ENABLE_OPENXR=1 TEST_WINDOWS_VULKAN=1)
    target_compile_features(mkw_openxr_vulkan_replay_tests PRIVATE cxx_std_17)
    set_target_properties(mkw_openxr_vulkan_replay_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_openxr_vulkan_replay_tests COMMAND mkw_openxr_vulkan_replay_tests)
endif()
