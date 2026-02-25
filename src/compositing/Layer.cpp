#include "compositing/Layer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

glm::mat3 Layer::getTransformMatrix() const {
    float rad = glm::radians(rotation);
    float c = std::cos(rad);
    float s = std::sin(rad);

    // Flip -> Scale -> Rotate -> Translate (composed as mat3)
    glm::mat3 m(1.0f);

    // Scale (with flip applied)
    m[0][0] = scale.x * (flipH ? -1.0f : 1.0f);
    m[1][1] = scale.y * (flipV ? -1.0f : 1.0f);

    // Rotate
    glm::mat3 rot(1.0f);
    rot[0][0] = c;  rot[1][0] = -s;
    rot[0][1] = s;  rot[1][1] = c;
    m = rot * m;

    // Translate
    m[2][0] = position.x;
    m[2][1] = position.y;

    return m;
}
