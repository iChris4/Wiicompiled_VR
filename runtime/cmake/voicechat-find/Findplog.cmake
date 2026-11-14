# See FindLibJuice.cmake: plog is added from its pinned source tree by cmake/VoiceChat.cmake.
if(TARGET plog::plog)
    set(plog_FOUND TRUE)
else()
    set(plog_FOUND FALSE)
endif()
