// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "macos_folder_dialog.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>

namespace whiteout::textool::views {

void ShowMacFolderDialogAllowingPackages(SDL_DialogFileCallback callback,
                                         void* userdata,
                                         SDL_Window* window,
                                         const char* default_location,
                                         bool allow_many) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    [panel setCanChooseFiles:NO];
    [panel setCanChooseDirectories:YES];
    [panel setAllowsMultipleSelection:(allow_many ? YES : NO)];
    // Key difference vs. SDL_ShowOpenFolderDialog: allow descending into
    // .app / file-package bundles so users can pick subfolders of e.g.
    // `Warcraft III.app/Contents/...`.
    [panel setTreatsFilePackagesAsDirectories:YES];

    if (default_location && *default_location) {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:default_location]];
        [panel setDirectoryURL:url];
    }

    NSWindow* parent_nswindow = nullptr;
    if (window) {
        parent_nswindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    }

    auto deliver = [callback, userdata](NSOpenPanel* p, NSInteger result) {
        if (result == NSModalResponseOK) {
            NSArray* urls = [p URLs];
            const NSUInteger n = [urls count];
            const char** files = (const char**)alloca(sizeof(const char*) * (n + 1));
            for (NSUInteger i = 0; i < n; ++i) {
                files[i] = [[[urls objectAtIndex:i] path] UTF8String];
            }
            files[n] = nullptr;
            callback(userdata, files, -1);
        } else {
            const char* empty[1] = {nullptr};
            callback(userdata, empty, -1);
        }
    };

    if (parent_nswindow) {
        [panel beginSheetModalForWindow:parent_nswindow
                      completionHandler:^(NSInteger result) {
            deliver(panel, result);
        }];
    } else {
        NSInteger result = [panel runModal];
        deliver(panel, result);
    }
}

} // namespace whiteout::textool::views

#endif // __APPLE__
