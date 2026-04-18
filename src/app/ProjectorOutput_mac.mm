#ifdef __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

void makeWindowTrulyBorderless(GLFWwindow* window) {
    NSWindow* nsWindow = (NSWindow*)glfwGetCocoaWindow(window);
    [nsWindow setStyleMask:NSWindowStyleMaskBorderless];
    [nsWindow setLevel:NSScreenSaverWindowLevel]; // Above everything including menu bar
    [nsWindow setHasShadow:NO];
    [nsWindow setCollectionBehavior:NSWindowCollectionBehaviorFullScreenAuxiliary];

    // Hide menu bar on the projector's screen
    [NSMenu setMenuBarVisible:NO];
}
#endif
