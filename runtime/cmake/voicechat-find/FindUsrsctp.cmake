# See FindLibJuice.cmake: usrsctp is built from its pinned source tree by cmake/VoiceChat.cmake.
if(TARGET Usrsctp::Usrsctp)
    set(Usrsctp_FOUND TRUE)
else()
    set(Usrsctp_FOUND FALSE)
endif()
