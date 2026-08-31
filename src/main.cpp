#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imnodes.h>
#include <tree_sitter/api.h>

// Declared in tree-sitter-python grammar
extern "C" const TSLanguage* tree_sitter_python();

#include "ui/ts_style.h"
#include "ui/ts_app.h"
#include "ui/ts_canvas.h"
#include "ui/ts_rail.h"
#include "cli/ts_cli.h"
#include "algo/AlgoRunner.h"
#include "algo/AlgoDbWriter.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef TELESCODE_STYLE_PREVIEW
#include "dev/style_preview.h"
#endif

static void PrintUsage()
{
    std::fprintf(stderr,
        "Telescode -- a telescope for your codebase.\n"
        "\n"
        "Usage:\n"
        "  Telescode [view] <db_path> [--no-algo]   open the viewer (default)\n"
        "  Telescode scan   <repo_path> <db_path> [allowed_root]\n"
        "  Telescode algo   <db_path>\n"
        "  Telescode update <op> <db_path> ...      (op: file|files|delete|rename|dangling)\n"
        "\n"
        "The viewer recomputes the reading sequence on every launch; --no-algo\n"
        "skips that and uses whatever the database already holds.\n");
}

int main(int argc, char** argv)
{
    // -- Subcommand dispatch ----------------------------------------------------
    // Each Cmd* was its own executable's main(). Shifting argv by one puts the
    // subcommand name in argv[0], so their existing index arithmetic still holds.
    if (argc > 1) {
        if (std::strcmp(argv[1], "scan")   == 0) return TS::CmdScan  (argc - 1, argv + 1);
        if (std::strcmp(argv[1], "algo")   == 0) return TS::CmdAlgo  (argc - 1, argv + 1);
        if (std::strcmp(argv[1], "update") == 0) return TS::CmdUpdate(argc - 1, argv + 1);
        if (std::strcmp(argv[1], "--help") == 0 ||
            std::strcmp(argv[1], "-h")     == 0) { PrintUsage(); return 0; }
    }

    // -- view mode (the default) ------------------------------------------------
    const char* db_path  = nullptr;
    bool        run_algo = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "view")      == 0) continue;   // optional, explicit
        if (std::strcmp(argv[i], "--no-algo") == 0) { run_algo = false; continue; }
        if (argv[i][0] == '-') {
            std::fprintf(stderr, "Telescode: unknown option '%s'\n\n", argv[i]);
            PrintUsage();
            return 1;
        }
        if (!db_path) db_path = argv[i];
    }

    // Recomputed on every launch rather than only when missing: it is well under
    // a second even on the largest database here (680 files / 50k links -> 0.8s),
    // and it rules out the rail showing a sequence that predates the last scan.
    // Runs before SDL comes up so a slow database cannot present a frozen window.
    // A read-only file or an out-of-date schema throws -- the viewer still opens
    // on whatever the database already holds, and the rail has an empty state.
    if (db_path && run_algo) {
        try {
            AlgoConfig    cfg    = AlgoDbWriter::loadConfig(db_path);
            AlgoRunResult result = AlgoRunner::run(db_path, cfg);
            // "computed", not "wrote": AlgoRunner returns the entries whether or
            // not AlgoDbWriter managed to persist them (a read-only file fails the
            // write and reports it on stderr), and claiming success here would
            // contradict that.
            std::fprintf(stdout, "[algo] computed %zu reading sequence entries\n",
                         result.entries.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[algo] skipped: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[algo] skipped: unknown error\n");
        }
    }

    // -- SDL init ---------------------------------------------------------------
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Telescode",
        1280, 900,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    // DPI scale -- set before LoadFonts so font sizes are correct
    TS::ui_scale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (TS::ui_scale < 0.5f) TS::ui_scale = 1.0f;
    SDL_SetWindowMinimumSize(window, 0, (int)(900.0f * TS::ui_scale));

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

    // -- ImGui + imnodes init --------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImNodes::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    TS::LoadFonts(io);   // must precede NewFrame
    TS::ApplyStyle();    // sets ImGuiStyle, ImNodesStyle, precomputes _U32

    // Load class diagram and reading sequence from DB if a path was given.
    if (db_path) {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            TS::InitCanvasFromDB(db);
            TS::InitRailFromDB(db);
        }
        sqlite3_close(db);
    }

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo gpu_info = {};
    gpu_info.Device            = gpu;
    gpu_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
    gpu_info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&gpu_info);

    // -- Main loop -------------------------------------------------------------
    bool running = true;

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

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        TS::DrawAppShell();

#ifdef TELESCODE_STYLE_PREVIEW
        TS::DrawStylePreview();
#endif

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
            target.texture     = swapchain;
            target.clear_color = { TS::BG.x, TS::BG.y, TS::BG.z, 1.0f };
            target.load_op     = SDL_GPU_LOADOP_CLEAR;
            target.store_op    = SDL_GPU_STOREOP_STORE;

            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass);
            SDL_EndGPURenderPass(pass);
        }

        SDL_SubmitGPUCommandBuffer(cmd);
    }

    // -- Cleanup ---------------------------------------------------------------
    SDL_WaitForGPUIdle(gpu);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    TS::ShutdownCanvas();           // free imnodes editor context before DestroyContext
    ImNodes::DestroyContext();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
