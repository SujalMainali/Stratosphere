#pragma once

#include <imgui.h>
#include <string>
#include <array>
#include <functional>

// MenuManager: draws a simple starting menu using ImGui.
//
// Usage:
//  - Create an instance in your Sample app (pass callbacks).
//  - Call menu.beginFrame/update/draw() inside the application's ImGui-enabled frame (we call from MySampleApp::OnRender).
//
// The MenuManager will try to load images by calling a user-provided loader callback (returns ImTextureID).
// If loader is not provided or fails, buttons render with text only.
//
// Buttons: New Game, Continue, Exit
class MenuManager
{
public:
    enum class Result
    {
        None,
        NewGame,
        ContinueGame,
        Exit
    };

    using TextureLoaderFn = std::function<ImTextureID(const std::string& path)>;

    MenuManager();
    ~MenuManager() = default;

    // Called once to give an optional texture loader.
    // loader should return an ImTextureID suitable for ImGui::Image / ImageButton (e.g. ImGui_ImplVulkan_AddTexture output)
    void SetTextureLoader(TextureLoaderFn loader);

    // Call in your ImGui frame (i.e. when ImGui::NewFrame() has been called).
    void OnImGuiFrame();

    // Returns the last activation result; after you handle it you can call ClearResult()
    Result GetResult() const { return m_result; }
    void ClearResult() { m_result = Result::None; }
    // Expose the texture loader so callers can query it.
    TextureLoaderFn GetTextureLoader() const { return m_loader; }

    // If a saved-game exists (file presence) this returns true.
    bool HasSaveFile() const { return m_hasSaveFile; }
    void SetHasSaveFile(bool v) { m_hasSaveFile = v; }

    // Controls to show/hide menu externally
    void Show() { m_show = true; m_timeSinceShown = 0.0f; }
    void Hide() { m_show = false; m_timeSinceShown = 0.0f; }
    bool IsVisible() const { return m_show; }

private:
    void handleInput();
    void drawMenu();

private:
    TextureLoaderFn m_loader = nullptr;
    std::array<ImTextureID, 3> m_tex = { nullptr, nullptr, nullptr }; // new, continue, exit
    ImTextureID m_background = nullptr;

    int m_selected = 0;
    bool m_show = true;
    bool m_hasSaveFile = false;
    float m_timeSinceShown = 0.0f;
    float m_fadeDuration = 0.4f; // seconds
    float m_alpha = 1.0f;

    Result m_result = Result::None;
};