#pragma once

#include <glm/glm.hpp>

namespace Engine
{
    // Simple frustum plane for culling (Ax + By + Cz + D = 0)
    struct FrustumPlane
    {
        glm::vec3 normal{0.0f};    // plane normal (should be normalized)
        float distance = 0.0f;      // D coefficient in plane equation

        // Test if a point is on the positive side of the plane
        float testPoint(const glm::vec3 &p) const
        {
            return glm::dot(normal, p) + distance;
        }

        // Test if a sphere is visible (fully or partially) on the positive side
        // Returns:
        //   > radius: fully inside (positive side)
        //   >= -radius: partially inside (intersecting or fully inside)
        //   < -radius: fully outside (negative side)
        float testSphere(const glm::vec3 &center, float radius) const
        {
            return testPoint(center) + radius;
        }
    };

    // Frustum defined by 6 planes (near, far, left, right, top, bottom)
    class Frustum
    {
    public:
        // Build frustum from OpenGL projection and view matrices
        // viewProj = projection * view
        static Frustum fromViewProjection(const glm::mat4 &viewProj)
        {
            Frustum f;

            // Extract planes from projection matrix:
            // The frustum planes in clip space can be extracted from the rows of the
            // combined view-projection matrix.

            // Helper to extract row from mat4
            auto getRow = [](const glm::mat4 &m, int r) -> glm::vec4
            { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };

            // Left plane: (1, 0, 0, 1) in clip space
            glm::vec4 left = getRow(viewProj, 3) + getRow(viewProj, 0);
            f.planes[static_cast<int>(PlaneIndex::Left)] = FrustumPlane{
                glm::normalize(glm::vec3(left)),
                left.w / glm::length(glm::vec3(left))};

            // Right plane: (1, 0, 0, -1) in clip space
            glm::vec4 right = getRow(viewProj, 3) - getRow(viewProj, 0);
            f.planes[static_cast<int>(PlaneIndex::Right)] = FrustumPlane{
                glm::normalize(glm::vec3(right)),
                right.w / glm::length(glm::vec3(right))};

            // Bottom plane: (0, 1, 0, 1) in clip space
            glm::vec4 bottom = getRow(viewProj, 3) + getRow(viewProj, 1);
            f.planes[static_cast<int>(PlaneIndex::Bottom)] = FrustumPlane{
                glm::normalize(glm::vec3(bottom)),
                bottom.w / glm::length(glm::vec3(bottom))};

            // Top plane: (0, 1, 0, -1) in clip space
            glm::vec4 top = getRow(viewProj, 3) - getRow(viewProj, 1);
            f.planes[static_cast<int>(PlaneIndex::Top)] = FrustumPlane{
                glm::normalize(glm::vec3(top)),
                top.w / glm::length(glm::vec3(top))};

            // Near plane: (0, 0, 1, 1) in clip space
            glm::vec4 near = getRow(viewProj, 3) + getRow(viewProj, 2);
            f.planes[static_cast<int>(PlaneIndex::Near)] = FrustumPlane{
                glm::normalize(glm::vec3(near)),
                near.w / glm::length(glm::vec3(near))};

            // Far plane: (0, 0, 1, -1) in clip space
            glm::vec4 far = getRow(viewProj, 3) - getRow(viewProj, 2);
            f.planes[static_cast<int>(PlaneIndex::Far)] = FrustumPlane{
                glm::normalize(glm::vec3(far)),
                far.w / glm::length(glm::vec3(far))};

            return f;
        }

        // Test if a sphere is visible in the frustum
        // Returns true if the sphere is visible (fully or partially in frustum)
        bool testSphere(const glm::vec3 &center, float radius) const
        {
            for (int i = 0; i < 6; ++i)
            {
                if (planes[i].testSphere(center, radius) < 0.0f)
                {
                    return false; // outside this plane, so not visible
                }
            }
            return true; // inside all planes
        }

    private:
        enum class PlaneIndex : int
        {
            Left = 0,
            Right = 1,
            Bottom = 2,
            Top = 3,
            Near = 4,
            Far = 5
        };

        FrustumPlane planes[6];
    };

} // namespace Engine
