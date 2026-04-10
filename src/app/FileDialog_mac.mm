#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#include <string>

std::string openFileDialog_mac(const char* filter) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];

        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [[panel URLs] objectAtIndex:0];
            return std::string([[url path] UTF8String]);
        }
    }
    return "";
}

std::string saveFileDialog_mac(const char* filter, const char* defaultExt) {
    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        if (defaultExt && strlen(defaultExt) > 0) {
            NSString* ext = [NSString stringWithUTF8String:defaultExt];
            [panel setAllowedContentTypes:@[]]; // Allow all, rely on extension
            [panel setNameFieldStringValue:[NSString stringWithFormat:@"untitled.%@", ext]];
        }

        if ([panel runModal] == NSModalResponseOK) {
            NSURL* url = [panel URL];
            return std::string([[url path] UTF8String]);
        }
    }
    return "";
}
#endif
