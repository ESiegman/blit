#pragma once

// The application around blit on macOS (src/macapp.m), from C and C++.

#ifdef __cplusplus
extern "C" {
#endif

// blit comes to the front (the running blit's side, when files are handed
// to it).
void blit_mac_activate(void);

// The blit of this program that is running is brought to the front by the
// system, as a click in the Dock would (the side of the `blit` run in a
// terminal; it has a say the running blit has not). Only when the program
// is inside an app bundle; false otherwise.
int blit_mac_front_bundle(const char* program);

// A notification from blit.app through the system's notification centre
// (asked leave to, the first time; waits for the answer, so not on the
// main thread). False when blit is no app bundle, or the system would not
// take it (then osascript's is the way, src/notify.cpp); true when it was
// shown, or notifications were turned off for blit.
int blit_mac_notify(const char* title, const char* body);

#ifdef __cplusplus
}
#endif
