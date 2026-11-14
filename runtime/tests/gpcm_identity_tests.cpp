// SPDX-License-Identifier: GPL-3.0-or-later
// The GPCM login observer: profile id from the "\lc\2\" reply, across split TCP segments, and the
// session's end on logout or socket close.

#include "voicechat/gpcm_identity.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using mkw::voicechat::GpcmIdentityTracker;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

std::span<const std::uint8_t> Span(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

const std::string kLoginReply =
    "\\lc\\2\\sesskey\\123456789\\proof\\0123456789abcdef\\userid\\11\\profileid\\600123456\\uniquenick\\"
    "7xyz\\lt\\abc__\\id\\1\\final\\";

void TestLogin() {
    GpcmIdentityTracker tracker;
    Check(!tracker.OnReceive(5, Span("\\lc\\1\\challenge\\ABCDEFGHIJ\\id\\1\\final\\")), "server challenge is not a login");
    Check(tracker.OnReceive(5, Span(kLoginReply)), "login reply changes the identity");
    Check(tracker.Current().online && tracker.Current().profileId == 600123456u, "profile id read");
    const auto generation = tracker.Current().generation;
    Check(!tracker.OnReceive(5, Span(kLoginReply)), "the same login again changes nothing");
    Check(tracker.Current().generation == generation, "generation stable for the same login");
    Check(tracker.OnSend(5, Span("\\logout\\\\sesskey\\123456789\\final\\")), "logout ends the session");
    Check(!tracker.Current().online && tracker.Current().profileId == 0, "offline after logout");
    Check(tracker.Current().generation > generation, "logout bumps the generation");
}

void TestSplitSegments() {
    GpcmIdentityTracker tracker;
    bool changed = false;
    for (std::size_t i = 0; i < kLoginReply.size(); i += 7) {
        changed = tracker.OnReceive(9, Span(kLoginReply.substr(i, 7))) || changed;
    }
    Check(changed && tracker.Current().profileId == 600123456u, "login reply split across segments");
    // Two messages in one segment, the second a keep-alive.
    GpcmIdentityTracker both;
    Check(both.OnReceive(3, Span(kLoginReply + "\\ka\\\\final\\")), "login followed by a keep-alive");
}

void TestCloseAndOtherSockets() {
    GpcmIdentityTracker tracker;
    tracker.OnReceive(5, Span(kLoginReply));
    Check(!tracker.OnClosed(6), "closing another socket keeps the login");
    Check(!tracker.OnSend(6, Span("\\logout\\\\final\\")), "a logout on another socket is not ours");
    Check(tracker.OnClosed(5), "closing the login socket ends the session");
    Check(!tracker.Current().online, "offline after close");
}

void TestValueParsing() {
    // A key's name inside another value must not match.
    const std::string message = "\\lc\\2\\uniquenick\\profileid\\profileid\\42\\final\\";
    Check(GpcmIdentityTracker::FindValue(message, "profileid") == "42", "keys are matched as keys only");
    Check(GpcmIdentityTracker::Command(message) == "lc", "command is the first key");
    GpcmIdentityTracker tracker;
    Check(!tracker.OnReceive(1, Span("\\lc\\2\\profileid\\0\\final\\")), "profile id 0 rejected");
    Check(!tracker.OnReceive(1, Span("\\lc\\2\\profileid\\99999999999\\final\\")), "oversized profile id rejected");
    Check(!tracker.OnReceive(1, Span("\\lc\\2\\profileid\\12x\\final\\")), "non-numeric profile id rejected");
    // Garbage without a terminator is bounded and does not stop later parsing.
    GpcmIdentityTracker noisy;
    noisy.OnReceive(2, Span(std::string(40000, 'x')));
    Check(noisy.OnReceive(2, Span(kLoginReply)), "login after garbage still parses");
}

} // namespace

int main() {
    TestLogin();
    TestSplitSegments();
    TestCloseAndOtherSockets();
    TestValueParsing();
    if (g_failures == 0) std::cout << "gpcm identity tests passed\n";
    return g_failures == 0 ? 0 : 1;
}
