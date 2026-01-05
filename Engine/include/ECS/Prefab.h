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
    - Prefab p = loadPrefabFromJson(text, registry, archetypes);
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

#include "ECS/Components.h"
#include "ECS/ArchetypeManager.h"

namespace Engine::ECS
{
    // Typed defaults per component ID. Extend as needed.
    using DefaultValue = std::variant<Position, Velocity, Health>;

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

    // Minimal JSON parsing (regex-based fallback; replace with a proper JSON library later if desired).
    // Expected JSON format example:
    // {
    //   "name": "TankBasic",
    //   "components": ["Position","Velocity","Health"],
    //   "defaults": {
    //       "Position": {"x":-0.8,"y":-0.8,"z":0.0},
    //       "Velocity": {"x":0.10,"y":0.05,"z":0.0},
    //       "Health":   {"value":100.0}
    //   }
    // }
    inline Prefab loadPrefabFromJson(const std::string &jsonText,
                                     ComponentRegistry &registry,
                                     ArchetypeManager &archetypes)
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

        // Resolve archetype
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