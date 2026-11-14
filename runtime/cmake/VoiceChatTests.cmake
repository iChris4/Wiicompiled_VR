# Headless contracts of the room voice chat (cmake/VoiceChat.cmake): MKW-VoiceChat wire
# compatibility, the GPCM login observer, the positional gains and the guest room reads. None of
# them opens a socket or an audio device, so they run on any machine and in CI.

set(MKW_VOICECHAT_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../tests")

foreach(test voice_protocol gpcm_identity)
    add_executable(mkw_${test}_tests "${MKW_VOICECHAT_TEST_DIR}/${test}_tests.cpp")
    target_link_libraries(mkw_${test}_tests PRIVATE mkw_voicechat_proto)
    set_target_properties(mkw_${test}_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_${test}_tests COMMAND mkw_${test}_tests)
endforeach()

add_executable(mkw_voice_spatial_tests "${MKW_VOICECHAT_TEST_DIR}/voice_spatial_tests.cpp")
target_include_directories(mkw_voice_spatial_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(mkw_voice_spatial_tests PRIVATE cxx_std_17)
add_test(NAME mkw_voice_spatial_tests COMMAND mkw_voice_spatial_tests)

add_executable(mkw_voice_room_tests "${MKW_VOICECHAT_TEST_DIR}/voice_room_tests.cpp")
target_include_directories(mkw_voice_room_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
target_compile_features(mkw_voice_room_tests PRIVATE cxx_std_17)
add_test(NAME mkw_voice_room_tests COMMAND mkw_voice_room_tests)

# The real peer transport over the loopback of this machine: libdatachannel, DTLS on mbed TLS and
# SCTP, with the MKWVC-ICE-1 bundles exchanged by hand. Host candidates only, no internet needed.
if(MKW_ENABLE_VOICECHAT)
    add_executable(mkw_voice_transport_smoke "${MKW_VOICECHAT_TEST_DIR}/voice_transport_smoke.cpp"
                                             "${CMAKE_CURRENT_LIST_DIR}/../voicechat/core/IcePeerTransport.cpp")
    target_include_directories(mkw_voice_transport_smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../voicechat/core")
    target_link_libraries(mkw_voice_transport_smoke PRIVATE mkw_voicechat_proto LibDataChannel::LibDataChannelStatic)
    set_target_properties(mkw_voice_transport_smoke PROPERTIES CXX_STANDARD 23 UNITY_BUILD OFF)
    add_test(NAME mkw_voice_transport_smoke COMMAND mkw_voice_transport_smoke)
endif()

# A whole session without the game, for a developer at a machine with audio devices: two probes
# against runtime/tests/voice_signaling_mock.py (see the probe's header). Not a CTest test.
if(MKW_ENABLE_VOICECHAT)
    add_executable(mkw_voice_session_probe "${MKW_VOICECHAT_TEST_DIR}/voice_session_probe.cpp")
    target_link_libraries(mkw_voice_session_probe PRIVATE mkw::voicechat)
    set_target_properties(mkw_voice_session_probe PROPERTIES UNITY_BUILD OFF)
endif()
