// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plays the game's part for the voice chat engine, so a whole session can run without Mario Kart
// Wii: it feeds a GPCM login and an RKNet room through voice_api.h exactly as the socket HLE and
// the frame hook do, then prints the session state once a second. Run two of them (different
// profile ids) against runtime/tests/voice_signaling_mock.py, or against a real signaling server.
//
//   mkw_voice_session_probe --pid 600000001 --peer 600000002 [--server ws://127.0.0.1:8787/]
//                           [--seconds 30] [--talk]
//
// Exits 0 once every listed peer is connected and its voice packets arrive (after lingering 5 s
// so the other probe can see it too). It opens this
// machine's default microphone and speakers (--talk sends the microphone continuously).

#include "voicechat/voice_api.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace api = mkw::voicechat;

int main(int argc, char** argv) {
    std::uint32_t pid = 0;
    std::vector<std::uint32_t> peers;
    std::string server = "ws://127.0.0.1:8787/";
    int seconds = 30;
    bool talk = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--pid") pid = static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10));
        else if (arg == "--peer") peers.push_back(static_cast<std::uint32_t>(std::strtoul(next(), nullptr, 10)));
        else if (arg == "--server") server = next();
        else if (arg == "--seconds") seconds = std::atoi(next());
        else if (arg == "--talk") talk = true;
    }
    if (pid == 0 || peers.empty()) {
        std::fprintf(stderr, "usage: %s --pid N --peer M [--peer ...] [--server URL] [--seconds S] [--talk]\n", argv[0]);
        return 2;
    }

    api::Settings settings;
    settings.enabled = true;
    settings.signalingUrl = server;
    settings.talkMode = talk ? api::TalkMode::OpenMic : api::TalkMode::VoiceActivation;
    settings.noiseSuppression = false;
    settings.positional = false;
    api::Start(settings);

    // The login reply the game receives from GPCM (port 29900), on a socket the game connected
    // nonblocking, so the HLE reports its peer port as 0.
    const std::string login = "\\lc\\2\\sesskey\\1\\proof\\0\\userid\\1\\profileid\\" + std::to_string(pid) +
                              "\\uniquenick\\probe\\lt\\x\\id\\1\\final\\";
    api::ObserveTcpConnect(7, 29900);
    api::ObserveTcpReceive(7, 0, reinterpret_cast<const std::uint8_t*>(login.data()), login.size());

    api::RoomSnapshot room;
    room.inRoom = true;
    room.roomKey = 0x1234;
    room.roomProfileIds = peers;
    room.roomProfileIds.push_back(pid);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int result = 1;
    int stableTicks = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        api::PostRoomSnapshot(room);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto status = api::GetStatus();
        std::size_t ready = 0;
        std::printf("[probe %u] %s | mic %s peak %u", pid, status.message.c_str(),
                    status.microphoneOpen ? "open" : "closed", status.microphonePeak);
        for (const auto& peer : status.peers) {
            std::printf(" | %s%s%s", peer.displayName.c_str(), peer.connected ? " connected" : " connecting",
                        peer.receiving ? " receiving" : "");
            if (peer.connected && peer.receiving) ++ready;
        }
        if (!status.audioError.empty()) std::printf(" | audio: %s", status.audioError.c_str());
        std::printf("\n");
        std::fflush(stdout);
        stableTicks = ready >= peers.size() ? stableTicks + 1 : 0;
        if (stableTicks >= 3 && result != 0) {
            // Stay a little longer so the other probe sees this one too before it leaves.
            result = 0;
            const auto linger = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (linger < deadline) {
                while (std::chrono::steady_clock::now() < linger) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            break;
        }
    }
    std::printf("[probe %u] %s\n", pid, result == 0 ? "PASS" : "FAIL");
    api::Shutdown();
    return result;
}
