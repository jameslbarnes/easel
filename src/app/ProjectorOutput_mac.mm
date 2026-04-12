#ifdef __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>

void makeWindowTrulyBorderless(GLFWwindow* window) {
    NSWindow* nsWindow = (NSWindow*)glfwGetCocoaWindow(window);
    [nsWindow setStyleMask:NSWindowStyleMaskBorderless];
    [nsWindow setLevel:NSNormalWindowLevel];
    [nsWindow setHasShadow:NO];
}
#endif
