#include "warp/HomographyUtils.h"
#include <cmath>
#include <array>

namespace HomographyUtils {

// Solve 8x8 system using Gaussian elimination
static bool solveLinearSystem(double A[8][9], double x[8]) {
    const int n = 8;

    // Forward elimination with partial pivoting
    for (int col = 0; col < n; col++) {
        // Find pivot
        int maxRow = col;
        double maxVal = std::abs(A[col][col]);
        for (int row = col + 1; row < n; row++) {
            if (std::abs(A[row][col]) > maxVal) {
                maxVal = std::abs(A[row][col]);
                maxRow = row;
            }
        }

        if (maxVal < 1e-12) return false; // singular

        // Swap rows
        if (maxRow != col) {
            for (int j = 0; j <= n; j++) {
                std::swap(A[col][j], A[maxRow][j]);
            }
        }

        // Eliminate below
        for (int row = col + 1; row < n; row++) {
            double factor = A[row][col] / A[col][col];
            for (int j = col; j <= n; j++) {
                A[row][j] -= factor * A[col][j];
            }
        }
    }

    // Back substitution
    for (int i = n - 1; i >= 0; i--) {
        x[i] = A[i][n];
        for (int j = i + 1; j < n; j++) {
            x[i] -= A[i][j] * x[j];
        }
        x[i] /= A[i][i];
    }

    return true;
}

glm::mat3 solve(const std::array<glm::vec2, 4>& src, const std::array<glm::vec2, 4>& dst) {
    // Set up 8x9 augmented matrix for Ax = b
    // Homography: [h0 h1 h2; h3 h4 h5; h6 h7 1] maps src -> dst
    // For each point pair (x,y) -> (u,v):
    //   u = (h0*x + h1*y + h2) / (h6*x + h7*y + 1)
    //   v = (h3*x + h4*y + h5) / (h6*x + h7*y + 1)
    // Rearranged:
    //   h0*x + h1*y + h2 - h6*x*u - h7*y*u = u
    //   h3*x + h4*y + h5 - h6*x*v - h7*y*v = v

    double A[8][9] = {};

    for (int i = 0; i < 4; i++) {
        double x = src[i].x, y = src[i].y;
        double u = dst[i].x, v = dst[i].y;

        int r0 = i * 2;
        int r1 = i * 2 + 1;

        A[r0][0] = x;  A[r0][1] = y;  A[r0][2] = 1;
        A[r0][3] = 0;  A[r0][4] = 0;  A[r0][5] = 0;
        A[r0][6] = -x * u; A[r0][7] = -y * u;
        A[r0][8] = u;

        A[r1][0] = 0;  A[r1][1] = 0;  A[r1][2] = 0;
        A[r1][3] = x;  A[r1][4] = y;  A[r1][5] = 1;
        A[r1][6] = -x * v; A[r1][7] = -y * v;
        A[r1][8] = v;
    }

    double h[8];
    if (!solveLinearSystem(A, h)) {
        return glm::mat3(1.0f); // identity fallback
    }

    // GLM mat3 is column-major
    // Our homography: [h0 h1 h2; h3 h4 h5; h6 h7 1]
    // In column-major: col0=[h0,h3,h6], col1=[h1,h4,h7], col2=[h2,h5,1]
    glm::mat3 H;
    H[0][0] = (float)h[0]; H[0][1] = (float)h[3]; H[0][2] = (float)h[6];
    H[1][0] = (float)h[1]; H[1][1] = (float)h[4]; H[1][2] = (float)h[7];
    H[2][0] = (float)h[2]; H[2][1] = (float)h[5]; H[2][2] = 1.0f;

    return H;
}

} // namespace HomographyUtils
