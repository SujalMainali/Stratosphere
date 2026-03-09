#pragma once

#include <cstdint>
#include <string>

namespace Engine::ECS
{
    struct ECSContext;
}

namespace Editor
{
    // Spawns entities described in the editor-owned GameWorld JSON.
    // Returns total number of spawned entities.
    uint32_t SpawnFromGameWorldFile(Engine::ECS::ECSContext &ecs, const std::string &path, bool selectSpawned = false);
}
