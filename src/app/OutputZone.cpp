#include "app/OutputZone.h"

bool OutputZone::init() {
    if (!compositor.init(width, height)) return false;
    if (!warpFBO.create(width, height, false)) return false;
    if (!readbackFBO.create(width, height, false)) return false;
    return true;
}

void OutputZone::resize(int w, int h) {
    if (w == width && h == height) return;
    width = w;
    height = h;
    compositor.resize(w, h);
    warpFBO.resize(w, h);
    readbackFBO.resize(w, h);
}
