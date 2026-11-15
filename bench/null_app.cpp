// The least an SDL_GPU program can do: start video, open a window, create a
// GPU device and present one cleared frame. Its launch time is the floor that
// blit's startup is measured against; everything above it is blit's own cost.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

int main(int argc, char** argv) {
    double launch_wall = 0;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--launch-wall")) launch_wall = std::atof(argv[i + 1]);
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    const double t_sdl = ms();
    SDL_Window* win = SDL_CreateWindow("null", 1100, 760, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const double t_window = ms();
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (!win || !dev || !SDL_ClaimWindowForGPUDevice(dev, win)) return 1;
    const double t_gpu = ms();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUTexture* tex = nullptr;
    SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &tex, nullptr, nullptr);
    if (tex) {
        SDL_GPUColorTargetInfo color{};
        color.texture = tex;
        color.clear_color = {0.11f, 0.11f, 0.13f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_EndGPURenderPass(SDL_BeginGPURenderPass(cmd, &color, 1, nullptr));
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    const double t_submit = ms();
    SDL_WaitForGPUIdle(dev);
    const double t_done = ms();
    timespec ts;
    std::timespec_get(&ts, TIME_UTC);
    const double wall = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;

    std::printf("{\"file\":\"\",\"bench\":\"startup_floor\",\"target\":\"window\",\"ms\":{\"sdl_init\":%.2f,\"window\":%.2f,"
                "\"gpu\":%.2f,\"first_frame_submit\":%.2f,\"first_frame_done\":%.2f}",
                t_sdl, t_window, t_gpu, t_submit, t_done);
    if (launch_wall > 0) std::printf(",\"exec_to_first_frame_ms\":%.2f", (wall - launch_wall) * 1e3);
    std::printf("}\n");
    return 0;
}
