#pragma once
/*
  Prefab.h
  --------
  Purpose:
    - Define Prefab (name, signature, archetypeId, typed defaults).
    - Define PrefabManager (dictionary keyed by name).
    - Provide JSON loader for Prefabs; constructs signature masks from ComponentRegistry,
      validates defaults, and resolves archetype via ArchetypeManager.

  Usage:
    - std::string text = readFileText("Sample/Entity.json");
    - Prefab p = loadPrefabFromJson(text, registry, archetypes, assets);
    - PrefabManager.add(p);
*/

#include <string>
#include <unordered_map>
#include <variant>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <regex>
#include <vector>
#include <iostream>

#include "ECS/Components.h"
#include "ECS/ArchetypeManager.h"

namespace Engine::ECS
{
    // Prefab: a template for spawning entities with a given component signature and default values.
    struct Prefab
    {
        std::string name;
        ComponentMask signature; // built from component IDs
        uint32_t archetypeId = UINT32_MAX;
        std::unordered_map<uint32_t, DefaultValue> defaults; // compId -> typed default

        // Validate that defaults only include components present in the signature.
        bool validateDefaults() const
        {
            for (const auto &kv : defaults)
            {
                const uint32_t cid = kv.first;
                if (!signature.has(cid))
                    return false; // default provided for a component not in signature
            }
            return true;
        }
    };

    // PrefabManager: dictionary keyed by prefab name.
    class PrefabManager
    {
    public:
        void add(const Prefab &p) { m_prefabs[p.name] = p; }

        const Prefab *get(const std::string &name) const
        {
            auto it = m_prefabs.find(name);
            return it != m_prefabs.end() ? &it->second : nullptr;
        }

        bool exists(const std::string &name) const
        {
            return m_prefabs.find(name) != m_prefabs.end();
        }

    private:
        std::unordered_map<std::string, Prefab> m_prefabs;
    };

    // Utility: read a whole file into a string.
    inline std::string readFileText(const std::string &path)
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // Helper: build a signature mask from component names via ComponentRegistry.
    inline ComponentMask buildSignatureFromNames(const std::vector<std::string> &names, ComponentRegistry &registry)
    {
        ComponentMask sig;
        for (const auto &n : names)
        {
            uint32_t id = registry.getId(n);
            if (id == ComponentRegistry::InvalidID)
                id = registry.registerComponent(n); // ensure presence in data-driven flow
            sig.set(id);
        }
        return sig;
    }

    inline Prefab loadPrefabFromJson(const std::string &jsonText,
                                     ComponentRegistry &registry,
                                     ArchetypeManager &archetypes,
                                     Engine::AssetManager &assets)
    {
        Prefab p;

        // Extract name
        {
            std::regex re_name(R"re("name"\s*:\s*"([^"]+)")re");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_name))
                p.name = m[1].str();
        }

        // Extract components array
        {
            std::regex re_components(R"re("components"\s*:\s*\[([^\]]+)\])re");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_components))
            {
                std::string inner = m[1].str();
                std::regex re_item(R"re("([^"]+)")re");
                auto it = std::sregex_iterator(inner.begin(), inner.end(), re_item);
                auto end = std::sregex_iterator();
                std::vector<std::string> names;
                for (; it != end; ++it)
                    names.push_back((*it)[1].str());
                p.signature = buildSignatureFromNames(names, registry);
            }
        }

        // Optional visuals: if a model is present, load it and apply a RenderMesh default.
        // JSON schema: "visual": { "model": "path" , ... }
        {
            std::regex re_model(R"re("visual"\s*:\s*\{[\s\S]*?"model"\s*:\s*"([^"]+)")re");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_model))
            {
                const std::string modelPath = m[1].str();
                if (!modelPath.empty())
                {
                    Engine::MeshHandle h = assets.loadMesh(modelPath);
                    if (h.isValid())
                    {
                        const uint32_t rmId = registry.ensureId("RenderMesh");
                        p.signature.set(rmId);

                        RenderMesh rm{};
                        rm.handle = h;
                        p.defaults[rmId] = rm;
                    }
                    else
                    {
                        std::cerr << "[Prefab] Warning: Failed to load model mesh: " << modelPath << " for prefab " << p.name << "\n";
                    }
                }
            }
        }

        // Resolve archetype (after any signature adjustments like RenderMesh)
        p.archetypeId = archetypes.getOrCreate(p.signature);

        // Parse defaults: Position
        {
            std::regex re_pos(R"("Position"\s*:\s*\{\s*"x"\s*:\s*([-+]?\d*\.?\d+),\s*"y"\s*:\s*([-+]?\d*\.?\d+),\s*"z"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_pos))
            {
                Position pos{};
                pos.x = std::stof(m[1].str());
                pos.y = std::stof(m[2].str());
                pos.z = std::stof(m[3].str());
                uint32_t cid = registry.ensureId("Position");
                p.defaults.emplace(cid, pos);
            }
        }

        // Parse defaults: Velocity
        {
            std::regex re_vel(R"("Velocity"\s*:\s*\{\s*"x"\s*:\s*([-+]?\d*\.?\d+),\s*"y"\s*:\s*([-+]?\d*\.?\d+),\s*"z"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_vel))
            {
                Velocity vel{};
                vel.x = std::stof(m[1].str());
                vel.y = std::stof(m[2].str());
                vel.z = std::stof(m[3].str());
                uint32_t cid = registry.ensureId("Velocity");
                p.defaults.emplace(cid, vel);
            }
        }

        // Parse defaults: Health
        {
            std::regex re_health(R"("Health"\s*:\s*\{\s*"value"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_health))
            {
                Health h{};
                h.value = std::stof(m[1].str());
                uint32_t cid = registry.ensureId("Health");
                p.defaults.emplace(cid, h);
            }
        }

        // Parse defaults: MoveTarget
        {
            std::regex re_target(R"("MoveTarget"\s*:\s*\{\s*"x"\s*:\s*([-+]?\d*\.?\d+),\s*"y"\s*:\s*([-+]?\d*\.?\d+),\s*"z"\s*:\s*([-+]?\d*\.?\d+),\s*"active"\s*:\s*(\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_target))
            {
                MoveTarget t{};
                t.x = std::stof(m[1].str());
                t.y = std::stof(m[2].str());
                t.z = std::stof(m[3].str());
                t.active = static_cast<uint8_t>(std::stoi(m[4].str()));
                uint32_t cid = registry.ensureId("MoveTarget");
                p.defaults.emplace(cid, t);
            }
        }

        // Parse defaults: MoveSpeed
        {
            std::regex re_speed(R"("MoveSpeed"\s*:\s*\{\s*"value"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_speed))
            {
                MoveSpeed s{};
                s.value = std::stof(m[1].str());
                uint32_t cid = registry.ensureId("MoveSpeed");
                p.defaults.emplace(cid, s);
            }
        }

        // Parse defaults: Radius
        {
            std::regex re_radius(R"("Radius"\s*:\s*\{\s*"r"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_radius))
            {
                Radius r{};
                r.r = std::stof(m[1].str());
                uint32_t cid = registry.ensureId("Radius");
                p.defaults.emplace(cid, r);
            }
        }

        // Parse defaults: Separation
        {
            std::regex re_sep(R"("Separation"\s*:\s*\{\s*"value"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_sep))
            {
                Separation s{};
                s.value = std::stof(m[1].str());
                uint32_t cid = registry.ensureId("Separation");
                p.defaults.emplace(cid, s);
            }
        }

        // Parse defaults: AvoidanceParams
        {
            std::regex re_ap(R"("AvoidanceParams"\s*:\s*\{\s*"strength"\s*:\s*([-+]?\d*\.?\d+),\s*"maxAccel"\s*:\s*([-+]?\d*\.?\d+),\s*"blend"\s*:\s*([-+]?\d*\.?\d+)\s*\})");
            std::smatch m;
            if (std::regex_search(jsonText, m, re_ap))
            {
                AvoidanceParams ap{};
                ap.strength = std::stof(m[1].str());
                ap.maxAccel = std::stof(m[2].str());
                ap.blend = std::stof(m[3].str());
                uint32_t cid = registry.ensureId("AvoidanceParams");
                p.defaults.emplace(cid, ap);
            }
        }

        // Validate defaults align with signature; drop mismatches to keep consistency.
        if (!p.validateDefaults())
        {
            for (auto it = p.defaults.begin(); it != p.defaults.end();)
            {
                if (!p.signature.has(it->first))
                    it = p.defaults.erase(it);
                else
                    ++it;
            }
        }

        return p;
    }

} // namespace Engine::ECS