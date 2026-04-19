#import <Foundation/Foundation.h>
#include <unistd.h>
#include <iostream>

// If launched from a .app bundle, the working directory is "/" (or wherever
// Finder/Dock launched us from). Relative resource loads like
// `loadFromFiles("shaders/foo.frag")` would fail. Set CWD to the bundle's
// Resources/ so existing relative paths keep working.
//
// When launched directly from a terminal (./build/Easel/Contents/MacOS/Easel
// or unbundled), the bundlePath check still detects the bundle and chdirs
// correctly. If running unbundled (no Easel.app wrapper), CWD is left alone.
void setBundleWorkingDir() {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* bundlePath = [bundle bundlePath];
    // mainBundle returns a path even for unbundled binaries (the executable
    // dir). Detect a real .app bundle by suffix.
    if (![bundlePath hasSuffix:@".app"]) return;

    NSString* resourcePath = [bundle resourcePath];
    if (chdir([resourcePath UTF8String]) != 0) {
        std::cerr << "[main] chdir to Resources failed: "
                  << [resourcePath UTF8String] << std::endl;
    }
}
