#pragma once
/*
  NavGrid.h
  ---------
  Purpose:
    - Stores the 2D walkability grid (blocked/open).
    - Provides coordinate mapping between world space and grid space.
    - Used by PathfindingSystem to plan paths.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class NavGrid
{
public:
    float cellSize = 2.0f;
    float worldMinX = 0.0f;
    float worldMinZ = 0.0f;
    int width = 0;
    int height = 0;

    // 0 = walkable, 1 = blocked
    std::vector<uint8_t> blocked;

    // Dirty flag — set true when obstacles change; NavGridBuilderSystem clears it after rebuild.
    bool dirty = true;

    void rebuild(float cSize, float minX, float minZ, float maxX, float maxZ)
    {
        cellSize = (cSize > 0.1f) ? cSize : 2.0f;
        worldMinX = minX;
        worldMinZ = minZ;

        float w = maxX - minX;
        float h = maxZ - minZ;

        width = static_cast<int>(std::ceil(w / cellSize));
        height = static_cast<int>(std::ceil(h / cellSize));

        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;

        blocked.assign(static_cast<size_t>(width * height), 0);
        dirty = true;
    }

    // Check if a straight line from (x0, z0) to (x1, z1) is clear of obstacles.
    // Coordinates are in WORLD space.
    bool lineCheck(float x0, float z0, float x1, float z1) const
    {
        int gx0 = worldToGridX(x0);
        int gz0 = worldToGridZ(z0);
        int gx1 = worldToGridX(x1);
        int gz1 = worldToGridZ(z1);

        int dx = std::abs(gx1 - gx0);
        int dz = std::abs(gz1 - gz0);
        int sx = (gx0 < gx1) ? 1 : -1;
        int sz = (gz0 < gz1) ? 1 : -1;
        int err = dx - dz;

        while (true)
        {
            if (!isWalkable(gx0, gz0))
                return false;

            if (gx0 == gx1 && gz0 == gz1)
                break;

            int e2 = 2 * err;
            if (e2 > -dz)
            {
                err -= dz;
                gx0 += sx;
            }
            if (e2 < dx)
            {
                err += dx;
                gz0 += sz;
            }
        }
        return true;
    }

    int worldToGridX(float wx) const
    {
        return static_cast<int>(std::floor((wx - worldMinX) / cellSize));
    }

    int worldToGridZ(float wz) const
    {
        return static_cast<int>(std::floor((wz - worldMinZ) / cellSize));
    }

    float gridToWorldX(int gx) const
    {
        return worldMinX + (static_cast<float>(gx) + 0.5f) * cellSize;
    }

    float gridToWorldZ(int gz) const
    {
        return worldMinZ + (static_cast<float>(gz) + 0.5f) * cellSize;
    }

    bool isValid(int gx, int gz) const
    {
        return gx >= 0 && gx < width && gz >= 0 && gz < height;
    }

    bool isWalkable(int gx, int gz) const
    {
        if (!isValid(gx, gz))
            return false;
        return blocked[gz * width + gx] == 0;
    }

    bool lineCheckGrid(int gx0, int gz0, int gx1, int gz1) const
    {
        int dx = std::abs(gx1 - gx0);
        int dz = std::abs(gz1 - gz0);
        int sx = (gx0 < gx1) ? 1 : -1;
        int sz = (gz0 < gz1) ? 1 : -1;
        int err = dx - dz;

        while (true)
        {
            if (!isWalkable(gx0, gz0))
                return false;
            if (gx0 == gx1 && gz0 == gz1)
                break;
            int e2 = 2 * err;
            bool stepX = (e2 > -dz);
            bool stepZ = (e2 < dx);

            // Diagonal step: verify both cardinal corner cells are walkable.
            // Without this, the line slips between diagonally-adjacent blocked
            // cells and falsely reports "clear" — the same check A* already does
            // in its neighbor expansion (blocked[idx(cx,nz)] || blocked[idx(nx,cz)]).
            if (stepX && stepZ)
            {
                if (!isWalkable(gx0 + sx, gz0) || !isWalkable(gx0, gz0 + sz))
                    return false;
            }

            if (stepX)
            {
                err -= dz;
                gx0 += sx;
            }
            if (stepZ)
            {
                err += dx;
                gz0 += sz;
            }
        }
        return true;
    }

    void markObstacle(float wx, float wz, float radius)
    {
        int gxMin = worldToGridX(wx - radius);
        int gxMax = worldToGridX(wx + radius);
        int gzMin = worldToGridZ(wz - radius);
        int gzMax = worldToGridZ(wz + radius);

        gxMin = std::max(0, gxMin);
        gxMax = std::min(width - 1, gxMax);
        gzMin = std::max(0, gzMin);
        gzMax = std::min(height - 1, gzMax);

        for (int gz = gzMin; gz <= gzMax; ++gz)
        {
            for (int gx = gxMin; gx <= gxMax; ++gx)
            {
                float cx = gridToWorldX(gx);
                float cz = gridToWorldZ(gz);
                float dx = cx - wx;
                float dz = cz - wz;
                if (dx * dx + dz * dz <= radius * radius)
                {
                    blocked[gz * width + gx] = 1;
                }
            }
        }
    }
};
