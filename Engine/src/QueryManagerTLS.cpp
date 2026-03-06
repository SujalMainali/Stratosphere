#include "ECS/QueryManager.h"

namespace Engine::ECS
{
    thread_local const char *QueryManager::t_currentSystemName = nullptr;
}
