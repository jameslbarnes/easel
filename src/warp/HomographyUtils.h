#pragma once
#include <glm/glm.hpp>
#include <array>

namespace HomographyUtils {

// Solve homography from 4 source points to 4 destination points
// Points are in NDC (-1 to 1)
glm::mat3 solve(const std::array<glm::vec2, 4>& src, const std::array<glm::vec2, 4>& dst);

}
