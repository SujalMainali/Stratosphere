cmake_minimum_required(VERSION 3.20)
project(Sample LANGUAGES CXX)

# Shared gameplay/runtime code (used by SampleApp and EditorApp)
add_library(SampleGame STATIC
    src/ScenarioSpawner.cpp
    src/update.cpp
)
target_link_libraries(SampleGame PUBLIC Engine)
target_link_libraries(SampleGame PUBLIC nlohmann_json::nlohmann_json)
target_include_directories(SampleGame PUBLIC ${CMAKE_SOURCE_DIR}/Engine/include)
target_include_directories(SampleGame PUBLIC ${CMAKE_SOURCE_DIR}/Sample)

add_executable(SampleApp
    src/main.cpp
    src/MySampleApp.cpp
    src/VerifyLoadSModel.cpp
)
target_link_libraries(SampleApp PRIVATE SampleGame)
target_include_directories(SampleApp PRIVATE ${CMAKE_SOURCE_DIR}/Engine/include)
target_include_directories(SampleApp PRIVATE ${CMAKE_SOURCE_DIR}/Sample)

# Option: run OBJ -> SMESH conversion for Sample assets during build
option(STRATO_PROCESS_SAMPLE_OBJ "Convert Sample OBJ assets to cooked SMESH during build" ON)

if (STRATO_PROCESS_SAMPLE_OBJ)
    add_custom_target(ProcessSampleOBJAssets
        COMMAND ObjToSMeshTool
            ${CMAKE_SOURCE_DIR}/Sample/assets/raw/ObjModels
            ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/ObjModels
        DEPENDS ObjToSMeshTool
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Conditioning Sample OBJ assets to SMESH"
    )
    add_dependencies(SampleApp ProcessSampleOBJAssets)

endif()

# ============================================================
# Option: run GLTF/GLB -> SMODEL conversion for Sample assets during build
# ============================================================
option(STRATO_PROCESS_SAMPLE_GLTF "Convert Sample glTF/glb assets to cooked SMODEL during build" ON)

set(SAMPLE_GLTF_RAW_DIR    ${CMAKE_SOURCE_DIR}/Sample/assets/raw/GltfModels)
set(SAMPLE_GLTF_COOKED_DIR ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/GltfModels)

if (STRATO_PROCESS_SAMPLE_GLTF)

    # Find all glTF/glb files recursively in raw dir
    file(GLOB_RECURSE SAMPLE_GLTF_FILES CONFIGURE_DEPENDS
        "${SAMPLE_GLTF_RAW_DIR}/*.gltf"
        "${SAMPLE_GLTF_RAW_DIR}/*.glb"
    )

    set(SAMPLE_COOKED_SMODEL_OUTPUTS "")

    foreach(GLTF_FILE ${SAMPLE_GLTF_FILES})

        # Get path relative to raw root
        file(RELATIVE_PATH REL_PATH ${SAMPLE_GLTF_RAW_DIR} ${GLTF_FILE})

        # REL_PATH might be: "Helmet/helmet.gltf"
        get_filename_component(REL_DIR ${REL_PATH} DIRECTORY)
        get_filename_component(BASE_NAME ${GLTF_FILE} NAME_WE)

        # Cooked output path:
        # Sample/assets/cooked/GlftModels/<REL_DIR>/<BASE_NAME>.smodel
        set(OUT_DIR  ${SAMPLE_GLTF_COOKED_DIR}/${REL_DIR})
        set(OUT_FILE ${OUT_DIR}/${BASE_NAME}.smodel)

        # Optional sidecar(s) to split a single long animation into multiple clips.
        # The tool checks for both:
        #   <file>.clips.json   (e.g. scene.gltf.clips.json)
        #   <stem>.clips.json   (e.g. scene.clips.json)
        get_filename_component(GLTF_DIR ${GLTF_FILE} DIRECTORY)
        set(CLIPS_SIDECAR_A "${GLTF_FILE}.clips.json")
        set(CLIPS_SIDECAR_B "${GLTF_DIR}/${BASE_NAME}.clips.json")
        set(EXTRA_COOK_DEPS "")
        if (EXISTS "${CLIPS_SIDECAR_A}")
            list(APPEND EXTRA_COOK_DEPS "${CLIPS_SIDECAR_A}")
        endif()
        if (EXISTS "${CLIPS_SIDECAR_B}")
            list(APPEND EXTRA_COOK_DEPS "${CLIPS_SIDECAR_B}")
        endif()

        add_custom_command(
 
            OUTPUT ${OUT_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}
            COMMAND GltfToSmodelTool
                "${GLTF_FILE}"
                "${OUT_FILE}"
            DEPENDS GltfToSmodelTool "${GLTF_FILE}" ${EXTRA_COOK_DEPS}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Cooking GLTF -> SMODEL: ${REL_PATH}"
            VERBATIM
        )


        # Copy raw assets (PNG images for menu buttons, etc.) to runtime output
        add_custom_command(TARGET SampleApp POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SampleApp>/assets/raw
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Sample/assets/raw
            $<TARGET_FILE_DIR:SampleApp>/assets/raw
            COMMENT "Copying raw assets (images) to runtime output"
        )


        list(APPEND SAMPLE_COOKED_SMODEL_OUTPUTS ${OUT_FILE})

    endforeach()

    # Custom target that cooks all glTF files
    add_custom_target(ProcessSampleGLTFAssets ALL
        DEPENDS ${SAMPLE_COOKED_SMODEL_OUTPUTS}
        COMMENT "Conditioning Sample GLTF assets to SMODEL"
    )

    add_dependencies(SampleApp ProcessSampleGLTFAssets)

endif()


# Copy cooked SMODEL assets next to the executable for relative imports.
# NOTE: A POST_BUILD copy only runs when SampleApp relinks; when only assets change,
# the executable might be up-to-date and the runtime assets would stay stale.
if (STRATO_PROCESS_SAMPLE_GLTF)
    add_custom_target(CopySampleSMODELAssets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SampleApp>/assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/GltfModels
            $<TARGET_FILE_DIR:SampleApp>/assets
        DEPENDS ProcessSampleGLTFAssets
        COMMENT "Copying Sample SMODEL assets to runtime output"
    )
    add_dependencies(SampleApp CopySampleSMODELAssets)
endif()


# Copy cooked SMESH assets next to the executable for relative imports
    add_custom_command(TARGET SampleApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:SampleApp>/assets/ObjModels
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Sample/assets/cooked/ObjModels
            $<TARGET_FILE_DIR:SampleApp>/assets/ObjModels
        COMMENT "Copying Sample SMESH assets to runtime output"
    )


## NOTE: Copying SMODEL assets is handled by the CopySampleSMODELAssets target above.

# Copy SPIR-V shaders next to the executable.
# Note: POST_BUILD only runs when the target relinks; shader-only changes can update
# Engine/shaders/*.spv without relinking SampleApp, leaving stale runtime shaders.
# Fix: always run a lightweight copy step as a build dependency.
file(GLOB SHADER_SPV CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/Engine/shaders/*.spv)

add_custom_target(CopySampleShaders
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:SampleApp>/shaders
    DEPENDS ${SHADER_SPV}
    COMMENT "Syncing SPIR-V shaders to SampleApp runtime output"
)

if (TARGET EngineShaders)
    add_dependencies(CopySampleShaders EngineShaders)
endif()

foreach(SPIRV ${SHADER_SPV})
    add_custom_command(TARGET CopySampleShaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SPIRV} $<TARGET_FILE_DIR:SampleApp>/shaders/
    )
endforeach()

add_dependencies(SampleApp CopySampleShaders)

# Copy scenario definition next to the executable.
add_custom_command(TARGET SampleApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Sample/Scinerio.json"
        $<TARGET_FILE_DIR:SampleApp>/Scinerio.json
    COMMENT "Copying scenario JSON: ${CMAKE_SOURCE_DIR}/Sample/Scinerio.json"
)

# Copy battle config next to the executable.
add_custom_command(TARGET SampleApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/Sample/BattleConfig.json"
        $<TARGET_FILE_DIR:SampleApp>/BattleConfig.json
    COMMENT "Copying battle config JSON: ${CMAKE_SOURCE_DIR}/Sample/BattleConfig.json"
)

# Copy all *.json in Sample/entities to runtime output (SampleApp/entities/)
file(GLOB_RECURSE SAMPLE_ENTITY_JSON_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Sample/entities/*.json"
)

add_custom_command(TARGET SampleApp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:SampleApp>/entities
    COMMENT "Preparing entities output directory"
)

foreach(JSON_FILE ${SAMPLE_ENTITY_JSON_FILES})
    add_custom_command(TARGET SampleApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${JSON_FILE}"
            $<TARGET_FILE_DIR:SampleApp>/entities/
        COMMENT "Copying entity JSON: ${JSON_FILE}"
    )
endforeach()

## Dependencies are vendored by the Engine target (glfw, nlohmann_json, imgui).
