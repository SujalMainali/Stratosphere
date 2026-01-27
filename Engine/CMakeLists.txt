cmake_minimum_required(VERSION 3.20)
project(Engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies: Vulkan (system) ---
find_package(Vulkan REQUIRED)

# --- Vendor GLFW as static using FetchContent ---
include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build libraries static" FORCE)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "GLFW docs" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "GLFW tests" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "GLFW examples" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "GLFW install target" FORCE)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.3.8
)
FetchContent_MakeAvailable(glfw)

include(FetchContent)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Dear ImGui via FetchContent ---
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.1
)
FetchContent_MakeAvailable(imgui)

# Build ImGui as a static library with Vulkan + GLFW backends
add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC Vulkan::Vulkan glfw)

# Suppress warnings for imgui
if (MSVC)
    target_compile_options(imgui_lib PRIVATE /W0)
else()
    target_compile_options(imgui_lib PRIVATE -w)
endif()

# --- Engine target ---
add_library(Engine STATIC
    src/Application.cpp
    src/VulkanContext.cpp
    src/GLFWWindow.cpp
    src/SwapChain.cpp
    src/Renderer.cpp
    src/TrianglesRenderPassModule.cpp
    src/MeshRenderPassModule.cpp
    src/GroundPlaneRenderPassModule.cpp
    src/Pipeline.cpp
    src/BufferUtils.cpp
    src/camera.cpp
    src/SMeshLoader.cpp
    src/AssetManager.cpp
    src/MeshAssets.cpp
    src/ImageUtils.cpp
    src/SModelLoader.cpp
    src/SModelRenderPassModule.cpp
    src/TextureAsset.cpp
    src/PerformanceMonitor.cpp
    src/ImGuiLayer.cpp
)

# --- Shaders: compile GLSL -> SPIR-V (optional but recommended) ---
find_program(GLSLC_EXECUTABLE glslc)
find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)

set(ENGINE_SHADER_DIR ${CMAKE_SOURCE_DIR}/Engine/shaders)
set(ENGINE_SHADER_SOURCES
    ${ENGINE_SHADER_DIR}/triangle.vert
    ${ENGINE_SHADER_DIR}/triangle.frag
    ${ENGINE_SHADER_DIR}/mesh.vert
    ${ENGINE_SHADER_DIR}/mesh.frag
    ${ENGINE_SHADER_DIR}/smodel.vert
    ${ENGINE_SHADER_DIR}/smodel.frag
)

set(ENGINE_SHADER_SPV)
foreach(SHADER ${ENGINE_SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(OUT_SPV ${ENGINE_SHADER_DIR}/${SHADER_NAME}.spv)
    list(APPEND ENGINE_SHADER_SPV ${OUT_SPV})

    if (GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT ${OUT_SPV}
            COMMAND ${GLSLC_EXECUTABLE} -o ${OUT_SPV} ${SHADER}
            DEPENDS ${SHADER}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Compiling shader ${SHADER_NAME} -> ${SHADER_NAME}.spv"
            VERBATIM
        )
    elseif (GLSLANG_VALIDATOR_EXECUTABLE)
        add_custom_command(
            OUTPUT ${OUT_SPV}
            COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V -o ${OUT_SPV} ${SHADER}
            DEPENDS ${SHADER}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Compiling shader ${SHADER_NAME} -> ${SHADER_NAME}.spv"
            VERBATIM
        )
    endif()
endforeach()

if (GLSLC_EXECUTABLE OR GLSLANG_VALIDATOR_EXECUTABLE)
    add_custom_target(EngineShaders ALL DEPENDS ${ENGINE_SHADER_SPV})
    add_dependencies(Engine EngineShaders)
else()
    message(WARNING "No shader compiler found (glslc or glslangValidator). Using prebuilt *.spv files in Engine/shaders.")
endif()

target_include_directories(Engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Engine
    PUBLIC
        Vulkan::Vulkan
        glm
        imgui_lib
    PRIVATE
        glfw
        nlohmann_json::nlohmann_json
)

if (UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(Engine PRIVATE Threads::Threads)
endif()

target_compile_features(Engine PUBLIC cxx_std_17)

if (MSVC)
    target_compile_options(Engine PRIVATE /W4 /permissive-)

    # Suppress noisy 3rd-party warnings (GLFW)
    target_compile_options(glfw PRIVATE /W0)
else()
    # Your code: strict
    target_compile_options(Engine PRIVATE -Wall -Wextra -Wpedantic)

    # GLFW: suppress the noisy ones
    target_compile_options(glfw PRIVATE
        -Wno-pedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-cast-function-type
    )
endif()

add_subdirectory(tools)