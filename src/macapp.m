#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

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

int blit_mac_notify(const char* title, const char* body) {
    @autoreleasepool {
        // (Outside an app bundle the notification centre has no app to show
        // it under, and raises an exception rather than say so.)
        NSBundle* main = [NSBundle mainBundle];
        if (main.bundleIdentifier == nil || ![[main.bundlePath pathExtension] isEqualToString:@"app"]) return 0;
        UNUserNotificationCenter* centre = [UNUserNotificationCenter currentNotificationCenter];
        __block int taken = 0;
        dispatch_semaphore_t asked = dispatch_semaphore_create(0);
        // (Asked once: after that the answer given is what this says, at once.)
        [centre requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                              completionHandler:^(BOOL granted, NSError* error) {
                                taken = granted ? 2 : error == nil ? 1 : 0;
                                dispatch_semaphore_signal(asked);
                              }];
        if (dispatch_semaphore_wait(asked, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC)) != 0) return 1;  // (still being asked)
        if (taken != 2) return taken;
        UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
        content.title = [NSString stringWithUTF8String:title] ?: @"blit";
        content.body = [NSString stringWithUTF8String:body] ?: @"";
        content.sound = [UNNotificationSound defaultSound];
        UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:[[NSUUID UUID] UUIDString] content:content trigger:nil];
        __block int shown = 1;
        dispatch_semaphore_t added = dispatch_semaphore_create(0);
        [centre addNotificationRequest:request
                 withCompletionHandler:^(NSError* error) {
                   shown = error == nil;
                   dispatch_semaphore_signal(added);
                 }];
        dispatch_semaphore_wait(added, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        return shown;
    }
}
