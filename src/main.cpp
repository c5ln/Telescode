#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imnodes.h>

#include "ui/ts_style.h"

int main(int, char**)
{
    // ── SDL init ──────────────────────────────────────────────────────────────
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Telescode — Design System Preview",
        1280, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    // DPI scale — set before LoadFonts so font sizes are correct
    TS::ui_scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (TS::ui_scale < 0.5f) TS::ui_scale = 1.0f; // guard against bad values

    SDL_GPUDevice* gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB,
        false, nullptr
    );
    if (!gpu) {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return 1;
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu, window)) {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetGPUSwapchainParameters(gpu, window,
        SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_MAILBOX);

    // ── ImGui + imnodes init ──────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImNodes::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Step 3: load fonts (must precede NewFrame)
    TS::LoadFonts(io);

    // Step 4: apply Warm Cream palette to ImGuiStyle + ImNodesStyle
    TS::ApplyStyle();

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo gpu_info = {};
    gpu_info.Device            = gpu;
    gpu_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    gpu_info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&gpu_info);

    // ── Main loop ─────────────────────────────────────────────────────────────
    bool running          = true;
    bool show_demo        = true;
    bool show_style_panel = true;

    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        // Frame start
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ── ImGui demo window — verifies full palette is applied ───────────────
        if (show_demo)
            ImGui::ShowDemoWindow(&show_demo);

        // ── Telescode palette preview panel ───────────────────────────────────
        if (show_style_panel) {
            ImGui::SetNextWindowSize(ImVec2(420.0f * TS::ui_scale, 0.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Warm Cream — Token Preview", &show_style_panel);

            ImGui::PushFont(TS::FONT_MONO);
            ImGui::TextDisabled("// ts_style.h  Warm Cream palette");
            ImGui::PopFont();
            ImGui::Separator();

            auto swatch = [](const char* name, ImVec4 col) {
                ImGui::ColorButton(name, col,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                    ImVec2(18, 18));
                ImGui::SameLine();
                ImGui::PushFont(TS::FONT_MONO);
                ImGui::TextUnformatted(name);
                ImGui::PopFont();
            };

            if (ImGui::CollapsingHeader("Background", ImGuiTreeNodeFlags_DefaultOpen)) {
                swatch("BG",      TS::BG);
                swatch("BG_SOFT", TS::BG_SOFT);
                swatch("PANEL",   TS::PANEL);
                swatch("PANEL_2", TS::PANEL_2);
                swatch("LINE",    TS::LINE);
            }
            if (ImGui::CollapsingHeader("Ink", ImGuiTreeNodeFlags_DefaultOpen)) {
                swatch("INK",   TS::INK);
                swatch("INK_2", TS::INK_2);
                swatch("INK_3", TS::INK_3);
                swatch("MUTED", TS::MUTED);
            }
            if (ImGui::CollapsingHeader("Header", ImGuiTreeNodeFlags_DefaultOpen)) {
                swatch("NIGHT",      TS::NIGHT);
                swatch("NIGHT_2",    TS::NIGHT_2);
                swatch("NIGHT_LINE", TS::NIGHT_LINE);
            }
            if (ImGui::CollapsingHeader("Accents", ImGuiTreeNodeFlags_DefaultOpen)) {
                swatch("ACCENT_PRIMARY",          TS::ACCENT_PRIMARY);
                swatch("ACCENT_PRIMARY_SUBTLE",   TS::ACCENT_PRIMARY_SUBTLE);
                swatch("ACCENT_SECONDARY",        TS::ACCENT_SECONDARY);
                swatch("ACCENT_SECONDARY_SUBTLE", TS::ACCENT_SECONDARY_SUBTLE);
                swatch("ACCENT_FOCUS",            TS::ACCENT_FOCUS);
                swatch("ACCENT_FOCUS_SUBTLE",     TS::ACCENT_FOCUS_SUBTLE);
            }
            if (ImGui::CollapsingHeader("Widgets", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushFont(TS::FONT_MEDIUM);
                ImGui::Text("Node Title  (FONT_MEDIUM)");
                ImGui::PopFont();
                ImGui::PushFont(TS::FONT_BASE);
                ImGui::Text("Body text  (FONT_BASE)");
                ImGui::PopFont();
                ImGui::PushFont(TS::FONT_SMALL);
                ImGui::TextDisabled("Subtitle / badge  (FONT_SMALL)");
                ImGui::PopFont();
                ImGui::PushFont(TS::FONT_MONO);
                ImGui::Text("+ createOrder(req): Order   (FONT_MONO)");
                ImGui::PopFont();
                ImGui::Spacing();
                static float f = 0.5f;
                ImGui::SliderFloat("Slider", &f, 0.0f, 1.0f);
                static bool chk = true;
                ImGui::Checkbox("Checkbox", &chk);
                ImGui::Button("Button");
                ImGui::SameLine();
                ImGui::Button("Hovered  →");
            }
            ImGui::End();
        }

        // ── Render ────────────────────────────────────────────────────────────
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool minimized  = draw_data->DisplaySize.x <= 0.0f ||
                                draw_data->DisplaySize.y <= 0.0f;

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);

        SDL_GPUTexture* swapchain = nullptr;
        SDL_AcquireGPUSwapchainTexture(cmd, window, &swapchain, nullptr, nullptr);

        if (swapchain && !minimized) {
            Imgui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd);

            SDL_GPUColorTargetInfo target = {};
            target.texture    = swapchain;
            target.clear_color = { TS::BG.x, TS::BG.y, TS::BG.z, 1.0f };
            target.load_op    = SDL_GPU_LOADOP_CLEAR;
            target.store_op   = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    SDL_WaitForGPUIdle(gpu);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImNodes::DestroyContext();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
