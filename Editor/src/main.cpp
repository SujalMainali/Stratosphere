#include "EditorApp.h"

#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        const std::string configPath = (argc >= 2)
                                           ? std::string(argv[1])
#if defined(STRATO_DEFAULT_GAME_WORLD_PATH)
                                           : std::string(STRATO_DEFAULT_GAME_WORLD_PATH);
#else
                                           : std::string("GameWorld.json");
#endif
        EditorApp app(configPath);
        app.Run();
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "Unhandled exception: " << e.what() << std::endl;
#endif
        return 1;
    }

    return 0;
}
