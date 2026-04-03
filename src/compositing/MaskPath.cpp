#include "compositing/MaskPath.h"
#include <cmath>

glm::vec2 MaskPath::evalBezier(glm::vec2 p0, glm::vec2 c0, glm::vec2 c1, glm::vec2 p1, float t) {
    float u = 1.0f - t;
    return u * u * u * p0 +
           3.0f * u * u * t * c0 +
           3.0f * u * t * t * c1 +
           t * t * t * p1;
}

std::vector<glm::vec2> MaskPath::tessellateSegment(
    glm::vec2 p0, glm::vec2 h0out, glm::vec2 h1in, glm::vec2 p1, int segments) {
    std::vector<glm::vec2> result;
    result.reserve(segments + 1);
    for (int i = 0; i <= segments; i++) {
        float t = (float)i / segments;
        result.push_back(evalBezier(p0, h0out, h1in, p1, t));
    }
    return result;
}

void MaskPath::addPoint(glm::vec2 pos) {
    MaskPoint pt;
    pt.position = pos;
    pt.handleIn = glm::vec2(0.0f);
    pt.handleOut = glm::vec2(0.0f);
    pt.smooth = true;
    m_points.push_back(pt);
    m_dirty = true;
}

void MaskPath::insertPoint(int afterIndex, float t) {
    if (afterIndex < 0 || afterIndex >= (int)m_points.size()) return;
    int nextIndex = (afterIndex + 1) % (int)m_points.size();

    auto& a = m_points[afterIndex];
    auto& b = m_points[nextIndex];

    // De Casteljau split at parameter t
    glm::vec2 p0 = a.position;
    glm::vec2 c0 = a.position + a.handleOut;
    glm::vec2 c1 = b.position + b.handleIn;
    glm::vec2 p1 = b.position;

    glm::vec2 q0 = glm::mix(p0, c0, t);
    glm::vec2 q1 = glm::mix(c0, c1, t);
    glm::vec2 q2 = glm::mix(c1, p1, t);

    glm::vec2 r0 = glm::mix(q0, q1, t);
    glm::vec2 r1 = glm::mix(q1, q2, t);

    glm::vec2 s = glm::mix(r0, r1, t); // point on curve

    // Update existing handles
    a.handleOut = q0 - a.position;
    b.handleIn = r1 - b.position;

    // Create new point
    MaskPoint newPt;
    newPt.position = s;
    newPt.handleIn = r0 - s;
    newPt.handleOut = r1 - s;
    newPt.smooth = true;

    m_points.insert(m_points.begin() + afterIndex + 1, newPt);
    m_dirty = true;
}

void MaskPath::removePoint(int index) {
    if (index < 0 || index >= (int)m_points.size()) return;
    m_points.erase(m_points.begin() + index);
    m_dirty = true;
}

std::vector<glm::vec2> MaskPath::tessellate(int segmentsPerCurve) const {
    std::vector<glm::vec2> result;
    if (m_points.size() < 2) {
        if (m_points.size() == 1) result.push_back(m_points[0].position);
        return result;
    }

    int n = (int)m_points.size();
    int edges = m_closed ? n : (n - 1);

    for (int i = 0; i < edges; i++) {
        int j = (i + 1) % n;
        glm::vec2 p0 = m_points[i].position;
        glm::vec2 c0 = p0 + m_points[i].handleOut;
        glm::vec2 c1 = m_points[j].position + m_points[j].handleIn;
        glm::vec2 p1 = m_points[j].position;

        auto seg = tessellateSegment(p0, c0, c1, p1, segmentsPerCurve);
        // Skip first point of each segment (except the first) to avoid duplicates
        for (int k = (i == 0 ? 0 : 1); k < (int)seg.size(); k++) {
            result.push_back(seg[k]);
        }
    }

    return result;
}

int MaskPath::hitTestPoint(glm::vec2 pos, float radius) const {
    float r2 = radius * radius;
    for (int i = 0; i < (int)m_points.size(); i++) {
        glm::vec2 d = pos - m_points[i].position;
        if (glm::dot(d, d) < r2) return i;
    }
    return -1;
}

int MaskPath::hitTestHandleIn(glm::vec2 pos, float radius) const {
    float r2 = radius * radius;
    for (int i = 0; i < (int)m_points.size(); i++) {
        if (glm::length(m_points[i].handleIn) < 0.001f) continue;
        glm::vec2 hpos = m_points[i].position + m_points[i].handleIn;
        glm::vec2 d = pos - hpos;
        if (glm::dot(d, d) < r2) return i;
    }
    return -1;
}

int MaskPath::hitTestHandleOut(glm::vec2 pos, float radius) const {
    float r2 = radius * radius;
    for (int i = 0; i < (int)m_points.size(); i++) {
        if (glm::length(m_points[i].handleOut) < 0.001f) continue;
        glm::vec2 hpos = m_points[i].position + m_points[i].handleOut;
        glm::vec2 d = pos - hpos;
        if (glm::dot(d, d) < r2) return i;
    }
    return -1;
}

int MaskPath::hitTestEdge(glm::vec2 pos, float radius, float& outT) const {
    if (m_points.size() < 2) return -1;

    int n = (int)m_points.size();
    int edges = m_closed ? n : (n - 1);
    float bestDist = radius;
    int bestEdge = -1;
    float bestT = 0.0f;

    for (int i = 0; i < edges; i++) {
        int j = (i + 1) % n;
        glm::vec2 p0 = m_points[i].position;
        glm::vec2 c0 = p0 + m_points[i].handleOut;
        glm::vec2 c1 = m_points[j].position + m_points[j].handleIn;
        glm::vec2 p1 = m_points[j].position;

        // Sample the curve at intervals to find closest point
        const int samples = 32;
        for (int k = 0; k <= samples; k++) {
            float t = (float)k / samples;
            glm::vec2 pt = evalBezier(p0, c0, c1, p1, t);
            float dist = glm::length(pos - pt);
            if (dist < bestDist) {
                bestDist = dist;
                bestEdge = i;
                bestT = t;
            }
        }
    }

    outT = bestT;
    return bestEdge;
}

glm::vec2 MaskPath::centroid() const {
    if (m_points.empty()) return glm::vec2(0.5f);
    glm::vec2 sum(0.0f);
    for (const auto& p : m_points) sum += p.position;
    return sum / (float)m_points.size();
}

void MaskPath::makeRectangle(glm::vec2 center, glm::vec2 size) {
    m_points.clear();
    float hw = size.x * 0.5f, hh = size.y * 0.5f;
    glm::vec2 corners[4] = {
        {center.x - hw, center.y - hh},
        {center.x + hw, center.y - hh},
        {center.x + hw, center.y + hh},
        {center.x - hw, center.y + hh},
    };
    for (auto& c : corners) {
        MaskPoint pt;
        pt.position = c;
        pt.handleIn = glm::vec2(0.0f);
        pt.handleOut = glm::vec2(0.0f);
        pt.smooth = false;
        m_points.push_back(pt);
    }
    m_closed = true;
    m_dirty = true;
}

void MaskPath::makeEllipse(glm::vec2 center, glm::vec2 size) {
    m_points.clear();
    // 4-point bezier circle approximation (kappa = 0.5522847498)
    const float k = 0.5522847498f;
    float rx = size.x * 0.5f, ry = size.y * 0.5f;
    float kx = k * rx, ky = k * ry;

    // Top, Right, Bottom, Left
    MaskPoint top;
    top.position = {center.x, center.y - ry};
    top.handleIn = {-kx, 0.0f};
    top.handleOut = {kx, 0.0f};
    top.smooth = true;

    MaskPoint right;
    right.position = {center.x + rx, center.y};
    right.handleIn = {0.0f, -ky};
    right.handleOut = {0.0f, ky};
    right.smooth = true;

    MaskPoint bottom;
    bottom.position = {center.x, center.y + ry};
    bottom.handleIn = {kx, 0.0f};
    bottom.handleOut = {-kx, 0.0f};
    bottom.smooth = true;

    MaskPoint left;
    left.position = {center.x - rx, center.y};
    left.handleIn = {0.0f, ky};
    left.handleOut = {0.0f, -ky};
    left.smooth = true;

    m_points = {top, right, bottom, left};
    m_closed = true;
    m_dirty = true;
}

void MaskPath::makeTriangle(glm::vec2 center, float radius) {
    m_points.clear();
    for (int i = 0; i < 3; i++) {
        float angle = (float)i * (2.0f * 3.14159265f / 3.0f) - 3.14159265f * 0.5f;
        MaskPoint pt;
        pt.position = {center.x + cosf(angle) * radius,
                       center.y + sinf(angle) * radius};
        pt.handleIn = glm::vec2(0.0f);
        pt.handleOut = glm::vec2(0.0f);
        pt.smooth = false;
        m_points.push_back(pt);
    }
    m_closed = true;
    m_dirty = true;
}

void MaskPath::makeStar(glm::vec2 center, float outerRadius, float innerRadius, int numPoints) {
    m_points.clear();
    const float pi = 3.14159265f;
    int total = numPoints * 2;
    for (int i = 0; i < total; i++) {
        float angle = (float)i * (2.0f * pi / total) - pi * 0.5f;
        float r = (i % 2 == 0) ? outerRadius : innerRadius;
        MaskPoint pt;
        pt.position = {center.x + cosf(angle) * r,
                       center.y + sinf(angle) * r};
        pt.handleIn = glm::vec2(0.0f);
        pt.handleOut = glm::vec2(0.0f);
        pt.smooth = false;
        m_points.push_back(pt);
    }
    m_closed = true;
    m_dirty = true;
}

void MaskPath::makePolygon(glm::vec2 center, float radius, int sides) {
    m_points.clear();
    const float pi = 3.14159265f;
    for (int i = 0; i < sides; i++) {
        float angle = (float)i * (2.0f * pi / sides) - pi * 0.5f;
        MaskPoint pt;
        pt.position = {center.x + cosf(angle) * radius,
                       center.y + sinf(angle) * radius};
        pt.handleIn = glm::vec2(0.0f);
        pt.handleOut = glm::vec2(0.0f);
        pt.smooth = false;
        m_points.push_back(pt);
    }
    m_closed = true;
    m_dirty = true;
}
