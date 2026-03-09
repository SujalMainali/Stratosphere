cmake_minimum_required(VERSION 3.20)
project(Editor LANGUAGES CXX)

add_executable(EditorApp
    src/main.cpp
    src/EditorApp.cpp
    src/BattleConfigEditor.cpp
    src/EditorRenderRunner.cpp
    src/EntityTypeEditor.cpp
    src/GameWorldSpawner.cpp
)

target_link_libraries(EditorApp PRIVATE Engine)
target_link_libraries(EditorApp PRIVATE nlohmann_json::nlohmann_json)

# Default paths (used when no CLI arg is provided)
target_compile_definitions(EditorApp PRIVATE
    STRATO_DEFAULT_GAME_WORLD_PATH="${CMAKE_SOURCE_DIR}/Editor/GameWorld.json"
    STRATO_EDITOR_ENTITIES_DIR="${CMAKE_SOURCE_DIR}/Editor/entities"
)

target_include_directories(EditorApp PRIVATE
    ${CMAKE_SOURCE_DIR}/Editor/include
    ${CMAKE_SOURCE_DIR}/Engine/include
)

# ------------------------------------------------------------
# Runtime data next to the executable (mirrors SampleApp)
# ------------------------------------------------------------

# Copy SPIR-V shaders to runtime output dir (expects Engine/shaders/*.spv present)
file(GLOB SHADER_SPV ${CMAKE_SOURCE_DIR}/Engine/shaders/*.spv)
foreach(SPIRV ${SHADER_SPV})
    add_custom_command(TARGET EditorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:EditorApp>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPIRV} $<TARGET_FILE_DIR:EditorApp>/shaders/
    )
endforeach()

# Copy editor GameWorld next to the executable.
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Editor/GameWorld.json"
        $<TARGET_FILE_DIR:EditorApp>/GameWorld.json
    COMMENT "Copying GameWorld JSON: ${CMAKE_SOURCE_DIR}/Editor/GameWorld.json"
)

# Copy all *.json in Editor/entities to runtime output (EditorApp/entities/)
file(GLOB_RECURSE EDITOR_ENTITY_JSON_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Editor/entities/*.json"
)

add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/entities
    COMMENT "Preparing entities output directory"
)

foreach(JSON_FILE ${EDITOR_ENTITY_JSON_FILES})
    add_custom_command(TARGET EditorApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JSON_FILE}"
            $<TARGET_FILE_DIR:EditorApp>/entities/
        COMMENT "Copying entity JSON: ${JSON_FILE}"
    )
endforeach()

# Raw assets (PNG images etc.)
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/assets/raw
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Editor/assets/raw
        $<TARGET_FILE_DIR:EditorApp>/assets/raw
    COMMENT "Copying raw assets (images) to runtime output"
)

# Copy cooked SMODEL assets next to the executable for relative imports
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Editor/assets/cooked/GltfModels
        $<TARGET_FILE_DIR:EditorApp>/assets
    COMMENT "Copying Editor SMODEL assets to runtime output"
)

# Copy cooked SMESH assets next to the executable for relative imports
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/assets/ObjModels
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Editor/assets/cooked/ObjModels
        $<TARGET_FILE_DIR:EditorApp>/assets/ObjModels
    COMMENT "Copying Editor SMESH assets to runtime output"
)
