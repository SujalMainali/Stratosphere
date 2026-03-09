cmake_minimum_required(VERSION 3.20)
project(Editor LANGUAGES CXX)

add_executable(EditorApp
    src/main.cpp
    src/EditorApp.cpp
    src/BattleConfigEditor.cpp
)

target_link_libraries(EditorApp PRIVATE SampleGame)

# Default config path (used when no CLI arg is provided)
target_compile_definitions(EditorApp PRIVATE
    STRATO_DEFAULT_BATTLE_CONFIG_PATH="${CMAKE_SOURCE_DIR}/Sample/BattleConfig.json"
)

target_include_directories(EditorApp PRIVATE
    ${CMAKE_SOURCE_DIR}/Editor/include
    ${CMAKE_SOURCE_DIR}/Engine/include
    ${CMAKE_SOURCE_DIR}/Sample
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

# Copy battle config next to the executable.
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Sample/BattleConfig.json"
        $<TARGET_FILE_DIR:EditorApp>/BattleConfig.json
    COMMENT "Copying battle config JSON: ${CMAKE_SOURCE_DIR}/Sample/BattleConfig.json"
)

# Copy all *.json in Sample/entities to runtime output (EditorApp/entities/)
file(GLOB_RECURSE SAMPLE_ENTITY_JSON_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Sample/entities/*.json"
)

add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/entities
    COMMENT "Preparing entities output directory"
)

foreach(JSON_FILE ${SAMPLE_ENTITY_JSON_FILES})
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
        ${CMAKE_SOURCE_DIR}/Sample/assets/raw
        $<TARGET_FILE_DIR:EditorApp>/assets/raw
    COMMENT "Copying raw assets (images) to runtime output"
)

# Cooked assets are produced by the Sample asset pipeline targets.
# If those targets exist, depend on them so EditorApp has the same runtime assets.
if (TARGET ProcessSampleGLTFAssets)
    add_dependencies(EditorApp ProcessSampleGLTFAssets)

    add_custom_target(CopyEditorSMODELAssets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:EditorApp>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/GltfModels
            $<TARGET_FILE_DIR:EditorApp>/assets
        DEPENDS ProcessSampleGLTFAssets
        COMMENT "Copying Sample SMODEL assets to Editor runtime output"
    )
    add_dependencies(EditorApp CopyEditorSMODELAssets)
endif()

if (TARGET ProcessSampleOBJAssets)
    add_dependencies(EditorApp ProcessSampleOBJAssets)
endif()

# Copy cooked SMESH assets next to the executable for relative imports
add_custom_command(TARGET EditorApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EditorApp>/assets/ObjModels
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/ObjModels
        $<TARGET_FILE_DIR:EditorApp>/assets/ObjModels
    COMMENT "Copying Sample SMESH assets to Editor runtime output"
)
