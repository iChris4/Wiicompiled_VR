# libdatachannel's find_package(LibJuice) lands here when cmake/VoiceChat.cmake builds libjuice
# from its own pinned source tree (USE_SYSTEM_JUICE). The target already exists; this module only
# reports it, so libdatachannel never looks for an installed copy.
if(TARGET LibJuice::LibJuice)
    set(LibJuice_FOUND TRUE)
    set(LibJuice_VERSION "1.7.2")
else()
    set(LibJuice_FOUND FALSE)
endif()
