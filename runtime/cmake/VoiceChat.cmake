# Room voice chat (runtime/voicechat), wire-compatible with MKW-VoiceChat
# (https://github.com/zurasaaa/MKW-VoiceChat, GPL-3.0). See docs/voice-chat.md.
#
# Two libraries come out of this file:
#   mkw_voicechat_proto  the dependency-free protocol pieces (voice packet header, ICE bundle,
#                        signaling replies, GPCM login parser). Built on every platform; the
#                        headless tests link it.
#   mkw_voicechat        the engine behind include/voicechat/voice_api.h. With
#                        MKW_ENABLE_VOICECHAT it links Opus, miniaudio, RNNoise and
#                        libdatachannel (libjuice, usrsctp, plog, mbed TLS); without it, the same
#                        API is a stub that reports Available() == false. The runtime glue calls
#                        the API unconditionally, so it carries no #if and its objects do not
#                        depend on the option.
#
# The engine is its own static library, outside runtime/src: the runtime's source glob would
# otherwise pull it into the unity build, the fast-math flags and the compile audits. Every
# dependency is static, so the Quest game kit copies the archives from the probe's link line with
# no extra rule. Windows builds run with FETCHCONTENT_FULLY_DISCONNECTED=ON against the offline
# set from Launcher/Prepare-Dependencies.ps1, which pins the same archives (Pins asserted there).

set(MKW_ENABLE_VOICECHAT_DEFAULT OFF)
if(MKW_PLATFORM_WINDOWS OR MKW_PLATFORM_ANDROID)
    set(MKW_ENABLE_VOICECHAT_DEFAULT ON)
endif()
option(MKW_ENABLE_VOICECHAT "Build room voice chat (Opus + WebRTC data channels)" ${MKW_ENABLE_VOICECHAT_DEFAULT})
unset(MKW_ENABLE_VOICECHAT_DEFAULT)

set(MKW_VOICECHAT_DIR "${CMAKE_CURRENT_LIST_DIR}/../voicechat")
set(MKW_VOICECHAT_PUBLIC_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../include")

add_library(mkw_voicechat_proto STATIC
    "${MKW_VOICECHAT_DIR}/proto/voice_protocol.cpp"
    "${MKW_VOICECHAT_DIR}/proto/gpcm_identity.cpp")
target_include_directories(mkw_voicechat_proto PUBLIC "${MKW_VOICECHAT_PUBLIC_INCLUDE}")
target_compile_features(mkw_voicechat_proto PUBLIC cxx_std_20)
set_target_properties(mkw_voicechat_proto PROPERTIES UNITY_BUILD OFF CXX_EXTENSIONS OFF)

if(NOT MKW_ENABLE_VOICECHAT)
    add_library(mkw_voicechat STATIC "${MKW_VOICECHAT_DIR}/service/voice_service_stub.cpp")
    target_include_directories(mkw_voicechat PUBLIC "${MKW_VOICECHAT_PUBLIC_INCLUDE}")
    target_compile_features(mkw_voicechat PUBLIC cxx_std_20)
    set_target_properties(mkw_voicechat PROPERTIES UNITY_BUILD OFF)
    add_library(mkw::voicechat ALIAS mkw_voicechat)
    message(STATUS "Voice chat: disabled (MKW_ENABLE_VOICECHAT=OFF)")
    return()
endif()

include(FetchContent)

# --- mbed TLS ---------------------------------------------------------------------------------
# libdatachannel's TLS/DTLS backend. Non-Windows builds already fetched it above for the guest
# network HLE; Windows keeps Schannel for that and fetches the same pin only for voice chat.
if(NOT TARGET MbedTLS::mbedtls)
    include("${CMAKE_CURRENT_LIST_DIR}/MbedTLSPin.cmake")
    FetchContent_Declare(mkw_mbedtls_upstream
        URL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${MKW_MBEDTLS_VERSION}/mbedtls-${MKW_MBEDTLS_VERSION}.tar.bz2"
        URL_HASH SHA256=${MKW_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mkw_mbedtls_upstream)
endif()
# libdatachannel's DTLS transport configures SRTP protection profiles even with media off, so the
# extension has to be compiled in. PUBLIC: ssl.h's structs depend on it, so every unit including
# it (network_ssl.cpp on Android among them) must agree.
target_compile_definitions(mbedtls PUBLIC MBEDTLS_SSL_DTLS_SRTP)
if(NOT TARGET MbedTLS::MbedTLS)
    # The umbrella name libdatachannel links (it skips its own find_package when it exists).
    # Imported, like the target its own FindMbedTLS would make: libdatachannel's install(EXPORT)
    # names imported dependencies but rejects in-tree ones that are in no export set.
    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED GLOBAL)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES
        INTERFACE_LINK_LIBRARIES "MbedTLS::mbedtls;MbedTLS::mbedx509;MbedTLS::mbedcrypto")
endif()

# --- Pinned sources -----------------------------------------------------------------------------
# The same versions MKW-VoiceChat 0.14.8 builds with; usrsctp and plog are the commits
# libdatachannel 0.24.5's own submodules point at.
FetchContent_Declare(mkw_opus
    URL "https://github.com/xiph/opus/archive/v1.6.1.tar.gz"
    URL_HASH SHA256=bf0b97ec7a65890b8db90ef94c4d6c18de12584c3085031953a10986f5917745
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)
FetchContent_Declare(mkw_miniaudio
    URL "https://github.com/mackron/miniaudio/archive/0.11.25.tar.gz"
    URL_HASH SHA256=b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mkw-no-cmake)
FetchContent_Declare(mkw_rnnoise
    URL "https://github.com/xiph/rnnoise/archive/refs/tags/v0.2.tar.gz"
    URL_HASH SHA256=e4a17eb1493b7551f9f7781d2acc67b8847d6ed7c4b64db0fd62c017021ae1ec
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mkw-no-cmake)
# RNNoise v0.2's speech model (the archive its download_model.sh names for that tag).
FetchContent_Declare(mkw_rnnoise_model
    URL "https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz"
    URL_HASH SHA256=4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mkw-no-cmake)
FetchContent_Declare(mkw_plog
    URL "https://github.com/SergiusTheBest/plog/archive/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef.tar.gz"
    URL_HASH SHA256=92a08bce559b5f28aa88d3fd9071567414b9f43a83fe2a05a6dd14f1da536072
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)
FetchContent_Declare(mkw_usrsctp
    URL "https://github.com/sctplab/usrsctp/archive/fec583d54493f879d2ae44a743423bf8a04371ab.tar.gz"
    URL_HASH SHA256=e5c114afe73c9a0ec419fab5f5b3f63f3ce57b09b90d06ea85e63dca8aed3e7c
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)
FetchContent_Declare(mkw_libjuice
    URL "https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.2.tar.gz"
    URL_HASH SHA256=75159867c4a5a689a6559e11aa0d30c9eba12ce73a4ae3d898b521467e1f635d
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)
FetchContent_Declare(mkw_libdatachannel
    URL "https://github.com/paullouisageneau/libdatachannel/archive/refs/tags/v0.24.5.tar.gz"
    URL_HASH SHA256=454537c3cd526bed935d847bb2dff4046f266eef84d43b2a5f2f2f293c0026f4
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    EXCLUDE_FROM_ALL)

block(SCOPE_FOR VARIABLES PROPAGATE mkw_miniaudio_SOURCE_DIR mkw_rnnoise_SOURCE_DIR mkw_rnnoise_model_SOURCE_DIR)
    # Normal variables, scoped to this block, so the subprojects' option() calls see them
    # (CMP0077) without leaking cache entries into aurora or the runtime.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    # plog and usrsctp still declare CMake 3.0 minimums, which CMake 4 rejects outright.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)

    # A release archive has neither .git nor package_version for Opus to read its version from.
    set(OPUS_PACKAGE_VERSION "1.6.1")
    set(OPUS_BUILD_SHARED_LIBRARY OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    FetchContent_MakeAvailable(mkw_opus mkw_miniaudio mkw_rnnoise mkw_rnnoise_model)

    set(PLOG_BUILD_SAMPLES OFF)
    set(PLOG_INSTALL OFF)
    set(PLOG_BUILD_TESTS OFF)
    # The values libdatachannel sets for its own usrsctp submodule: SCTP over its DTLS
    # transport only (AF_CONN), no raw IPv4/IPv6 sockets.
    set(sctp_build_programs OFF)
    set(sctp_build_shared_lib OFF)
    set(sctp_build_fuzzer OFF)
    set(sctp_debug OFF)
    set(sctp_inet OFF)
    set(sctp_inet6 OFF)
    set(sctp_werror OFF)
    set(NO_SERVER ON)
    set(NO_TESTS ON)
    set(USE_NETTLE OFF)
    FetchContent_MakeAvailable(mkw_plog mkw_usrsctp mkw_libjuice)
    if(MINGW)
        target_compile_definitions(usrsctp PUBLIC "SCTP_STDINT_INCLUDE=<stdint.h>")
    endif()
    target_compile_options(usrsctp PRIVATE -w)
    target_compile_options(juice PRIVATE -w)
    if(NOT TARGET Usrsctp::Usrsctp)
        # Imported for the same install(EXPORT) reason as MbedTLS::MbedTLS above. (libjuice is in
        # its own export set already, and plog is linked as a build-only interface.)
        add_library(Usrsctp::Usrsctp INTERFACE IMPORTED GLOBAL)
        set_target_properties(Usrsctp::Usrsctp PROPERTIES INTERFACE_LINK_LIBRARIES usrsctp)
    endif()

    # The three submodules come from the trees above (FindLibJuice/FindUsrsctp/Findplog in
    # cmake/voicechat-find report the existing targets), and media transport is off: voice
    # travels as MKW-VoiceChat's own Opus packets over a data channel, so libSRTP is unused.
    list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/voicechat-find")
    set(USE_MBEDTLS ON)
    set(USE_GNUTLS OFF)
    set(USE_NICE OFF)
    set(USE_SYSTEM_JUICE ON)
    set(USE_SYSTEM_USRSCTP ON)
    set(USE_SYSTEM_PLOG ON)
    set(USE_SYSTEM_SRTP OFF)
    set(USE_SYSTEM_JSON OFF)
    set(NO_MEDIA ON)
    set(NO_WEBSOCKET OFF)
    set(NO_EXAMPLES ON)
    set(WARNINGS_AS_ERRORS OFF)
    set(RTC_UPDATE_VERSION_HEADER OFF)
    FetchContent_MakeAvailable(mkw_libdatachannel)
    target_compile_options(datachannel-static PRIVATE -w)
endblock()

# --- miniaudio -----------------------------------------------------------------------------------
# One implementation unit with only the backends this project uses: WASAPI on Windows, AAudio
# (loaded at run time, so nothing is linked) with OpenSL ES as its fallback on Android.
add_library(mkw_miniaudio STATIC "${MKW_VOICECHAT_DIR}/third_party/miniaudio_impl.c")
target_include_directories(mkw_miniaudio SYSTEM PUBLIC "${mkw_miniaudio_SOURCE_DIR}")
target_compile_definitions(mkw_miniaudio PUBLIC
    MA_NO_DECODING MA_NO_ENCODING MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE
    MA_NO_GENERATION MA_ENABLE_ONLY_SPECIFIC_BACKENDS)
if(MKW_PLATFORM_WINDOWS)
    target_compile_definitions(mkw_miniaudio PUBLIC MA_ENABLE_WASAPI)
    target_link_libraries(mkw_miniaudio PUBLIC ole32)
elseif(MKW_PLATFORM_ANDROID)
    target_compile_definitions(mkw_miniaudio PUBLIC MA_ENABLE_AAUDIO MA_ENABLE_OPENSL)
    target_link_libraries(mkw_miniaudio PUBLIC OpenSLES)
endif()
target_compile_options(mkw_miniaudio PRIVATE -w)
set_target_properties(mkw_miniaudio PROPERTIES UNITY_BUILD OFF C_EXTENSIONS ON)

# --- RNNoise -------------------------------------------------------------------------------------
# The library sources of the v0.2 tag plus its model. DISABLE_DEBUG_FLOAT leaves out the model's
# float copies of the quantized weights, which only the training tools read.
set(MKW_RNNOISE_SRC "${mkw_rnnoise_SOURCE_DIR}/src")
add_library(mkw_rnnoise STATIC
    "${MKW_RNNOISE_SRC}/denoise.c"
    "${MKW_RNNOISE_SRC}/rnn.c"
    "${MKW_RNNOISE_SRC}/rnnoise_tables.c"
    "${MKW_RNNOISE_SRC}/nnet.c"
    "${MKW_RNNOISE_SRC}/nnet_default.c"
    "${MKW_RNNOISE_SRC}/parse_lpcnet_weights.c"
    "${MKW_RNNOISE_SRC}/pitch.c"
    "${MKW_RNNOISE_SRC}/celt_lpc.c"
    "${MKW_RNNOISE_SRC}/kiss_fft.c"
    "${mkw_rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c")
target_include_directories(mkw_rnnoise
    PUBLIC "${mkw_rnnoise_SOURCE_DIR}/include"
    PRIVATE "${mkw_rnnoise_model_SOURCE_DIR}/src" "${MKW_RNNOISE_SRC}"
            "${MKW_VOICECHAT_DIR}/third_party/rnnoise")
target_compile_definitions(mkw_rnnoise PRIVATE RNNOISE_BUILD _USE_MATH_DEFINES DISABLE_DEBUG_FLOAT)
target_compile_options(mkw_rnnoise PRIVATE -w)
set_target_properties(mkw_rnnoise PROPERTIES UNITY_BUILD OFF C_STANDARD 99 C_EXTENSIONS ON)
if(NOT MKW_PLATFORM_WINDOWS)
    target_link_libraries(mkw_rnnoise PUBLIC m)
endif()

# --- The engine ----------------------------------------------------------------------------------
add_library(mkw_voicechat STATIC
    "${MKW_VOICECHAT_DIR}/core/AdaptiveJitterBuffer.cpp"
    "${MKW_VOICECHAT_DIR}/core/AudioEngine.cpp"
    "${MKW_VOICECHAT_DIR}/core/AudioProcessor.cpp"
    "${MKW_VOICECHAT_DIR}/core/IcePeerTransport.cpp"
    "${MKW_VOICECHAT_DIR}/core/OpusCodec.cpp"
    "${MKW_VOICECHAT_DIR}/core/SignalingClient.cpp"
    "${MKW_VOICECHAT_DIR}/core/VoiceClient.cpp"
    "${MKW_VOICECHAT_DIR}/service/voice_service.cpp")
target_include_directories(mkw_voicechat
    PUBLIC "${MKW_VOICECHAT_PUBLIC_INCLUDE}"
    PRIVATE "${MKW_VOICECHAT_DIR}/core")
target_compile_features(mkw_voicechat PUBLIC cxx_std_20)
set_target_properties(mkw_voicechat PROPERTIES CXX_STANDARD 23 CXX_EXTENSIONS OFF UNITY_BUILD OFF)
target_link_libraries(mkw_voicechat
    PUBLIC mkw_voicechat_proto
    PRIVATE mkw_rnnoise mkw_miniaudio Opus::opus LibDataChannel::LibDataChannelStatic)
if(MKW_PLATFORM_WINDOWS)
    target_compile_definitions(mkw_voicechat PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(mkw_voicechat PRIVATE bcrypt)
elseif(MKW_PLATFORM_ANDROID)
    target_link_libraries(mkw_voicechat PRIVATE log)
endif()
add_library(mkw::voicechat ALIAS mkw_voicechat)
message(STATUS "Voice chat: enabled (Opus, miniaudio, RNNoise, libdatachannel on mbed TLS)")
