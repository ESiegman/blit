#import <AppKit/AppKit.h>

#include "macapp.h"

void blit_mac_activate(void) {
    if (NSApp == nil) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [NSApp activateIgnoringOtherApps:YES];  // (only a request since macOS 14: the bundle's way below is the one that counts)
#pragma clang diagnostic pop
}

int blit_mac_front_bundle(const char* program) {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:program];
        // .../blit.app/Contents/MacOS/blit
        NSString* macos = [path stringByDeletingLastPathComponent];
        NSString* contents = [macos stringByDeletingLastPathComponent];
        NSString* bundle = [contents stringByDeletingLastPathComponent];
        if (![[macos lastPathComponent] isEqualToString:@"MacOS"] || ![[contents lastPathComponent] isEqualToString:@"Contents"] ||
            ![[bundle pathExtension] isEqualToString:@"app"])
            return 0;
        NSWorkspaceOpenConfiguration* config = [NSWorkspaceOpenConfiguration configuration];
        config.activates = YES;
        config.addsToRecentItems = NO;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:bundle]
                                              configuration:config
                                          completionHandler:^(NSRunningApplication* app, NSError* error) {
                                            (void)app;
                                            (void)error;
                                            dispatch_semaphore_signal(done);
                                          }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        return 1;
    }
}
