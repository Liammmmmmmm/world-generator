// wview: interactive preview of a generated world, stage by stage.
//
//   wview <config.json | world.wgw>
//
// With a config file, the whole pipeline runs in-process, every intermediate
// stage can be inspected, and everything is regenerated each time the file is
// saved, for a fast tweak-and-look loop.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include "Meshes.hpp"
#include "wg/core/Config.hpp"
#include "wg/gen/Pipeline.hpp"
#include "wg/io/WorldFile.hpp"
#include "wg/render/MapRenderer.hpp"

namespace fs = std::filesystem;

namespace {

constexpr Color kBackground{24, 26, 30, 255};
constexpr Color kText{230, 230, 230, 255};
constexpr Color kDimText{150, 150, 155, 255};
constexpr Color kPanel{0, 0, 0, 170};
constexpr Color kHighlight{70, 110, 200, 200};
constexpr Color kError{255, 100, 90, 255};

constexpr int kFont = 18;
constexpr int kLine = 22;
constexpr int kPanelWidth = 330;
constexpr int kMargin = 8;

enum class ViewMode { Map2D, Terrain3D };

class Viewer {
public:
    explicit Viewer(fs::path source) : source_(std::move(source)), isConfig_(source_.extension() == ".json") {}

    ~Viewer() { clearGpuCache(); }

    void run() {
        reload();
        while (!WindowShouldClose()) {
            handleInput();
            watchSource();

            BeginDrawing();
            ClearBackground(kBackground);
            if (hasView()) {
                if (mode_ == ViewMode::Map2D)
                    drawMap();
                else
                    drawTerrain();
            }
            drawPanel();
            drawStatusBar();
            EndDrawing();
        }
    }

private:
    // --- Data ----------------------------------------------------------------

    const wg::Snapshot& current() const { return snapshots_[selected_]; }
    const wg::WorldState& state() const { return current().state; }

    bool hasView() const {
        if (snapshots_.empty())
            return false;
        const auto views = wg::availableViews(state());
        return std::find(views.begin(), views.end(), view_) != views.end();
    }

    void reload() {
        try {
            const auto start = std::chrono::steady_clock::now();
            std::vector<wg::Snapshot> snapshots;
            if (isConfig_) {
                const auto config = wg::WorldConfig::load(source_);
                snapshots = wg::Pipeline::fromConfig(config).run(config.info());
            } else {
                wg::Snapshot snapshot;
                snapshot.state = wg::readWorld(source_);
                snapshot.stageName = source_.filename().string();
                snapshot.stageType = "file";
                snapshot.changedLayers = snapshot.state.layerNames();
                snapshot.voxelsChanged = snapshot.state.hasVoxels();
                snapshots.push_back(std::move(snapshot));
            }
            const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;

            const bool firstLoad = snapshots_.empty();
            const auto& newInfo = snapshots.back().state.info();
            const bool resized =
                !firstLoad && (newInfo.width != state().info().width || newInfo.length != state().info().length);

            // Cached GPU resources point into the old snapshots.
            clearGpuCache();
            snapshots_ = std::move(snapshots);
            const int last = static_cast<int>(snapshots_.size()) - 1;
            selectStage(firstLoad ? last : std::min(selected_, last), /*keepView=*/!firstLoad);
            if (firstLoad || resized)
                resetCameras();

            status_ = (isConfig_ ? "Generated in " : "Loaded in ") + std::to_string(static_cast<int>(elapsed.count())) +
                      " ms";
            statusIsError_ = false;
        } catch (const std::exception& e) {
            // Keep showing the previous result so a typo in the config is not disruptive.
            status_ = e.what();
            statusIsError_ = true;
        }
        std::error_code ec;
        lastWrite_ = fs::last_write_time(source_, ec);
    }

    void watchSource() {
        if (!isConfig_ || GetTime() - lastWatchCheck_ < 0.5)
            return;
        lastWatchCheck_ = GetTime();
        std::error_code ec;
        const auto writeTime = fs::last_write_time(source_, ec);
        if (!ec && writeTime != lastWrite_)
            reload();
    }

    void selectStage(int index, bool keepView = false) {
        selected_ = std::clamp(index, 0, static_cast<int>(snapshots_.size()) - 1);
        const auto views = wg::availableViews(state());
        if (!keepView || std::find(views.begin(), views.end(), view_) == views.end())
            view_ = wg::defaultView(current());
    }

    void cycleView() {
        const auto views = wg::availableViews(state());
        if (views.empty())
            return;
        auto it = std::find(views.begin(), views.end(), view_);
        view_ = (it == views.end() || ++it == views.end()) ? views.front() : *it;
    }

    // --- GPU resources (cached per data, mode and color map) -----------------

    struct GpuEntry {
        Texture2D texture{};
        Model model{};
    };
    using GpuKey = std::tuple<const void*, ViewMode, int>;

    const GpuEntry& gpuResource() {
        const bool voxels = view_.kind == wg::View::Kind::Voxels;
        const void* data = voxels ? state().voxelsIdentity() : state().layerIdentity(view_.layer);
        const GpuKey key{data, mode_, voxels ? -1 : static_cast<int>(colorMap_)};
        if (const auto it = gpuCache_.find(key); it != gpuCache_.end())
            return it->second;

        GpuEntry entry;
        if (mode_ == ViewMode::Map2D) {
            const auto image = wg::renderView(state(), view_, colorMap_);
            const Image rlImage{const_cast<std::uint8_t*>(image.pixels.data()), image.width, image.height, 1,
                                PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            entry.texture = LoadTextureFromImage(rlImage);
            SetTextureFilter(entry.texture, TEXTURE_FILTER_POINT);
        } else if (voxels) {
            entry.model = buildVoxelModel(state().voxels());
        } else {
            const auto& grid = state().layer(view_.layer);
            const auto range = wg::valueRange(grid);
            const int sea = state().info().seaLevel;
            const bool terrain = colorMap_ == wg::ColorMap::Terrain;
            entry.model = buildHeightfieldModel(
                grid.width(), grid.length(),
                [&](int x, int z) { return terrain ? std::max(grid.at(x, z), static_cast<float>(sea)) : grid.at(x, z); },
                [&](int x, int z) { return wg::layerColor(grid.at(x, z), colorMap_, range, sea); });
        }
        return gpuCache_.emplace(key, entry).first->second;
    }

    void clearGpuCache() {
        for (auto& [key, entry] : gpuCache_) {
            if (std::get<ViewMode>(key) == ViewMode::Map2D)
                UnloadTexture(entry.texture);
            else
                UnloadModel(entry.model);
        }
        gpuCache_.clear();
    }

    // --- Cameras & input -----------------------------------------------------

    void resetCameras() {
        const auto& info = state().info();
        const float w = static_cast<float>(info.width), l = static_cast<float>(info.length);
        const float viewWidth = static_cast<float>(GetScreenWidth() - kPanelWidth - 2 * kMargin);

        camera2D_.target = {w * 0.5f, l * 0.5f};
        camera2D_.offset = {kPanelWidth + kMargin + viewWidth * 0.5f, GetScreenHeight() * 0.5f};
        camera2D_.rotation = 0.0f;
        camera2D_.zoom = 0.9f * std::min(viewWidth / w, GetScreenHeight() / l);

        orbitTarget_ = {w * 0.5f, static_cast<float>(info.seaLevel), l * 0.5f};
        orbitYaw_ = 0.8f;
        orbitPitch_ = 0.6f;
        orbitDistance_ = std::max(w, l) * 1.1f;
        rlSetClipPlanes(1.0, std::max(4000.0, 6.0 * std::max(w, l)));
    }

    bool mouseOverPanel() const {
        const Vector2 m = GetMousePosition();
        return m.x < kPanelWidth + kMargin && m.y < panelHeight_ + kMargin;
    }

    void handleInput() {
        if (IsKeyPressed(KEY_R))
            reload();
        if (snapshots_.empty())
            return;

        if (IsKeyPressed(KEY_TAB))
            mode_ = mode_ == ViewMode::Map2D ? ViewMode::Terrain3D : ViewMode::Map2D;
        if (IsKeyPressed(KEY_F))
            resetCameras();
        if (IsKeyPressed(KEY_L))
            cycleView();
        if (IsKeyPressed(KEY_C))
            colorMap_ = static_cast<wg::ColorMap>((static_cast<int>(colorMap_) + 1) % static_cast<int>(wg::ColorMap::Count));
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_RIGHT))
            selectStage(selected_ + 1);
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_LEFT))
            selectStage(selected_ - 1);
        for (int key = KEY_ONE; key <= KEY_NINE; ++key)
            if (IsKeyPressed(key) && key - KEY_ONE < static_cast<int>(snapshots_.size()))
                selectStage(key - KEY_ONE);

        // Clicking a stage in the panel; dragging only starts outside of it.
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            dragging_ = !mouseOverPanel();
            const Vector2 m = GetMousePosition();
            const int row = static_cast<int>(std::floor((m.y - stageListTop_) / kLine));
            if (!dragging_ && m.y >= stageListTop_ && row < static_cast<int>(snapshots_.size()))
                selectStage(row);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            dragging_ = false;

        if (mode_ == ViewMode::Map2D)
            updateMapCamera();
        else
            updateOrbitCamera();
    }

    void updateMapCamera() {
        if (dragging_)
            camera2D_.target = Vector2Subtract(camera2D_.target, Vector2Scale(GetMouseDelta(), 1.0f / camera2D_.zoom));
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel()) {
            // Zoom around the cursor.
            const Vector2 mouse = GetMousePosition();
            camera2D_.target = GetScreenToWorld2D(mouse, camera2D_);
            camera2D_.offset = mouse;
            camera2D_.zoom = std::clamp(camera2D_.zoom * std::exp(wheel * 0.15f), 0.02f, 64.0f);
        }
    }

    void updateOrbitCamera() {
        const Vector2 delta = GetMouseDelta();
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            orbitYaw_ -= delta.x * 0.005f;
            orbitPitch_ = std::clamp(orbitPitch_ + delta.y * 0.005f, 0.05f, 1.5f);
        }
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            // Pan on the horizontal plane.
            const float speed = orbitDistance_ * 0.0015f;
            const Vector3 right{std::cos(orbitYaw_), 0.0f, -std::sin(orbitYaw_)};
            const Vector3 forward{-std::sin(orbitYaw_), 0.0f, -std::cos(orbitYaw_)};
            orbitTarget_ = Vector3Add(orbitTarget_, Vector3Scale(right, -delta.x * speed));
            orbitTarget_ = Vector3Add(orbitTarget_, Vector3Scale(forward, delta.y * speed));
        }
        if (const float wheel = GetMouseWheelMove(); wheel != 0.0f && !mouseOverPanel())
            orbitDistance_ = std::clamp(orbitDistance_ * std::exp(-wheel * 0.1f), 5.0f, 20000.0f);
    }

    Camera3D orbitCamera() const {
        Camera3D camera{};
        camera.target = orbitTarget_;
        camera.position = Vector3Add(orbitTarget_, {orbitDistance_ * std::cos(orbitPitch_) * std::sin(orbitYaw_),
                                                    orbitDistance_ * std::sin(orbitPitch_),
                                                    orbitDistance_ * std::cos(orbitPitch_) * std::cos(orbitYaw_)});
        camera.up = {0.0f, 1.0f, 0.0f};
        camera.fovy = 50.0f;
        camera.projection = CAMERA_PERSPECTIVE;
        return camera;
    }

    // --- Drawing -------------------------------------------------------------

    void drawMap() {
        const auto& info = state().info();
        const Texture2D& texture = gpuResource().texture;
        BeginMode2D(camera2D_);
        DrawTexture(texture, 0, 0, WHITE);
        DrawRectangleLinesEx({0, 0, static_cast<float>(info.width), static_cast<float>(info.length)},
                             2.0f / camera2D_.zoom, GRAY);
        EndMode2D();
    }

    void drawTerrain() {
        const Model& model = gpuResource().model;
        BeginMode3D(orbitCamera());
        DrawModel(model, {0, 0, 0}, 1.0f, WHITE);
        EndMode3D();
    }

    void drawPanel() {
        const int x = kMargin + 10;
        int y = kMargin + 8;
        const auto text = [&](const std::string& s, Color color = kText) {
            DrawText(s.c_str(), x, y, kFont, color);
            y += kLine;
        };

        DrawRectangle(kMargin, kMargin, kPanelWidth, panelHeight_, kPanel);
        text(source_.filename().string() + (isConfig_ ? "  (live reload)" : ""));
        if (!snapshots_.empty()) {
            const auto& info = state().info();
            text(TextFormat("%d x %d   y %d / %d   sea %d", info.width, info.length, info.minY, info.maxY,
                            info.seaLevel),
                 kDimText);
            y += 6;
            text("Stages  (up/down, 1-9, click)", kDimText);
            stageListTop_ = static_cast<float>(y);
            for (int i = 0; i < static_cast<int>(snapshots_.size()); ++i) {
                const auto& s = snapshots_[i];
                if (i == selected_)
                    DrawRectangle(kMargin + 4, y - 2, kPanelWidth - 8, kLine, kHighlight);
                DrawText(TextFormat("%d. %s", i + 1, s.stageName.c_str()), x, y, kFont, kText);
                const char* time = TextFormat("%.0f ms", s.seconds * 1000.0);
                DrawText(time, kMargin + kPanelWidth - 12 - MeasureText(time, kFont), y, kFont, kDimText);
                y += kLine;
            }
            y += 6;

            std::string views;
            for (const auto& v : wg::availableViews(state()))
                views += v == view_ ? "[" + v.label() + "] " : v.label() + " ";
            text("View (L): " + views);
            if (view_.kind == wg::View::Kind::Layer)
                text("Colors (C): " + std::string(wg::colorMapName(colorMap_)));
            else if (mode_ == ViewMode::Terrain3D)
                text(TextFormat("1 cube = %d blocks (LOD)", voxelLodStep(state().voxels())), kDimText);
            y += 6;
            text(mode_ == ViewMode::Map2D ? "2D: drag pan, wheel zoom" : "3D: left orbit, right pan, wheel", kDimText);
            text("TAB 2D/3D   R reload   F reset view", kDimText);
        }
        panelHeight_ = y - kMargin + 4;
    }

    void drawStatusBar() {
        const int h = kLine + 8;
        const int y = GetScreenHeight() - h;
        DrawRectangle(0, y, GetScreenWidth(), h, kPanel);
        const std::string hover = hoverInfo();
        DrawText(hover.c_str(), 12, y + 6, kFont, kText);
        const int statusX = std::max(12 + MeasureText(hover.c_str(), kFont) + 30,
                                     GetScreenWidth() - MeasureText(status_.c_str(), kFont) - 12);
        DrawText(status_.c_str(), statusX, y + 6, kFont, statusIsError_ ? kError : kDimText);
    }

    std::string hoverInfo() const {
        if (mode_ != ViewMode::Map2D || !hasView() || mouseOverPanel())
            return "";
        const Vector2 p = GetScreenToWorld2D(GetMousePosition(), camera2D_);
        const int x = static_cast<int>(std::floor(p.x)), z = static_cast<int>(std::floor(p.y));
        const auto& info = state().info();
        if (x < 0 || z < 0 || x >= info.width || z >= info.length)
            return "";
        if (view_.kind == wg::View::Kind::Voxels) {
            const auto& voxels = state().voxels();
            const int top = voxels.topNonAir(x, z);
            return TextFormat("x %d  z %d   top y %d  %s", x, z, top,
                              std::string(wg::blockInfo(voxels.get(x, top, z)).name).c_str());
        }
        return TextFormat("x %d  z %d   %s = %.2f", x, z, view_.layer.c_str(), state().layer(view_.layer).at(x, z));
    }

    // --- State ---------------------------------------------------------------

    fs::path source_;
    bool isConfig_;
    fs::file_time_type lastWrite_{};
    double lastWatchCheck_ = 0.0;

    std::vector<wg::Snapshot> snapshots_;
    int selected_ = 0;
    wg::View view_;
    wg::ColorMap colorMap_ = wg::ColorMap::Terrain;
    std::string status_;
    bool statusIsError_ = false;

    std::map<GpuKey, GpuEntry> gpuCache_;

    ViewMode mode_ = ViewMode::Map2D;
    Camera2D camera2D_{};
    Vector3 orbitTarget_{};
    float orbitYaw_ = 0.0f;
    float orbitPitch_ = 0.0f;
    float orbitDistance_ = 100.0f;
    bool dragging_ = false;

    float stageListTop_ = 0.0f;
    int panelHeight_ = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: wview <config.json | world.wgw>\n");
        return 2;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 800, "World Generator Viewer");
    {
        Viewer viewer(argv[1]);
        viewer.run();
    }
    CloseWindow();
    return 0;
}
