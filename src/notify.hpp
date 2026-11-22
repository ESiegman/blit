#pragma once
// A notification of the system's own, for what ends while blit is behind
// another window (a task, for now). On macOS, blit.app's own through the
// notification centre (UserNotifications, src/macapp.m); a blit that is no
// app bundle (a build folder's), through osascript's display notification.
// On Linux, the desktop's, freedesktop's Notify over D-Bus: through gdbus,
// else notify-send, found on PATH (no D-Bus library is linked). Handed over
// on a thread of its own: the caller never waits.
#include <string>
#include <vector>

namespace blit {

void system_notify(const std::string& title, const std::string& body);
// The program that hands one over, with its words, as found on PATH now
// (empty: there is none; on macOS, what is used outside an app bundle).
std::vector<std::string> notify_command(const std::string& title, const std::string& body);
// (Tests.) Those still being handed over, waited for (5 s at most).
void system_notify_wait();

}  // namespace blit
