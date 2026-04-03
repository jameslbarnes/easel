#pragma once
#include <glm/glm.hpp>
#include <vector>

struct MaskPoint {
    glm::vec2 position;       // anchor (UV space 0-1)
    glm::vec2 handleIn;       // incoming handle, relative to position
    glm::vec2 handleOut;      // outgoing handle, relative to position
    bool smooth = true;       // linked handles (moving one mirrors the other)
};

class MaskPath {
public:
    std::vector<MaskPoint>& points() { return m_points; }
    const std::vector<MaskPoint>& points() const { return m_points; }

    bool empty() const { return m_points.empty(); }
    int count() const { return (int)m_points.size(); }
    bool closed() const { return m_closed; }
    void setClosed(bool c) { m_closed = c; m_dirty = true; }

    bool isDirty() const { return m_dirty; }
    void clearDirty() { m_dirty = false; }
    void markDirty() { m_dirty = true; }

    // Add a point at the end. If drag is true, initial handles are set for dragging.
    void addPoint(glm::vec2 pos);

    // Insert a point on the edge between index and (index+1), at parameter t
    void insertPoint(int afterIndex, float t);

    // Remove point at index
    void removePoint(int index);

    // Tessellate the full path into line segments
    std::vector<glm::vec2> tessellate(int segmentsPerCurve = 24) const;

    // Tessellate a single segment (from point[i] to point[(i+1)%n])
    static std::vector<glm::vec2> tessellateSegment(
        glm::vec2 p0, glm::vec2 h0out, glm::vec2 h1in, glm::vec2 p1, int segments = 24);

    // Evaluate cubic bezier at t
    static glm::vec2 evalBezier(glm::vec2 p0, glm::vec2 c0, glm::vec2 c1, glm::vec2 p1, float t);

    // Hit testing (returns index, or -1)
    int hitTestPoint(glm::vec2 pos, float radius = 0.03f) const;
    int hitTestHandleIn(glm::vec2 pos, float radius = 0.02f) const;
    int hitTestHandleOut(glm::vec2 pos, float radius = 0.02f) const;

    // Find the nearest edge and parameter for point insertion
    // Returns edge index, sets outT to the parameter
    int hitTestEdge(glm::vec2 pos, float radius, float& outT) const;

    // Compute centroid of all points
    glm::vec2 centroid() const;

    // Shape presets (replace current points, centered at cx,cy with given size in UV space)
    void makeRectangle(glm::vec2 center, glm::vec2 size);
    void makeEllipse(glm::vec2 center, glm::vec2 size);
    void makeTriangle(glm::vec2 center, float radius);
    void makeStar(glm::vec2 center, float outerRadius, float innerRadius, int points = 5);
    void makePolygon(glm::vec2 center, float radius, int sides);

private:
    std::vector<MaskPoint> m_points;
    bool m_closed = true;
    bool m_dirty = true;
};
