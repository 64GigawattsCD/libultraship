#include "fast/Fast3dGui.h"

#include "fast/Fast3dWindow.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "fast/backends/gfx_metal.h"
#include "fast/interpreter.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/resource/type/Texture.h"
#include "ship/window/gui/resource/GuiTextureFactory.h"
#include "ship/resource/File.h"

#ifdef __APPLE__
#include <SDL_hints.h>
#include <SDL_video.h>
#include <imgui_impl_metal.h>
#include <imgui_impl_sdl2.h>
#else
#include <SDL2/SDL_hints.h>
#include <SDL2/SDL_video.h>
#endif

#if defined(__ANDROID__) || defined(__IOS__)
#include "ship/port/mobile/MobileImpl.h"
#endif

#ifdef ENABLE_OPENGL
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>
#endif

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

// NOLINTNEXTLINE
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>
#ifdef ENABLE_DX11
#include <d3d11.h>
#include <d3dcompiler.h>
#endif

namespace {
struct ArcadeKartPostFxView {
    int playerIndex;
    ImVec2 minUv;
    ImVec2 maxUv;
    ImVec2 minPos;
    ImVec2 maxPos;
};

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
struct ArcadeKartScenePostFxShaderConstants {
    float uvMinX;
    float uvMinY;
    float uvSizeX;
    float uvSizeY;
    float barrelStrength;
    float overscanPercent;
    float blurStrength;
    float padding0;
    float shakeUvX;
    float shakeUvY;
    float padding1;
    float padding2;
};

struct ArcadeKartScenePostFxDrawData {
    ArcadeKartScenePostFxShaderConstants constants;
};

static bool ArcadeKartShouldUseDx11PostFxShader();
static void ArcadeKartDrawDx11PostFxView(ImDrawList* drawList, ImTextureID textureId,
                                         const ArcadeKartPostFxView& view,
                                         const ArcadeKartScenePostFxShaderConstants& constants);
#endif

static float ArcadeKartClamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

static float ArcadeKartNormalizeRange(float value, float minValue, float maxValue) {
    const float range = maxValue - minValue;
    if (std::abs(range) < 0.0001f) {
        return value >= maxValue ? 1.0f : 0.0f;
    }
    return ArcadeKartClamp01((value - minValue) / range);
}

static float ArcadeKartApplyPostFxCurve(float value, float responsePower) {
    return std::pow(ArcadeKartClamp01(value), std::max(responsePower, 0.01f));
}

static float ArcadeKartLerp(float a, float b, float t) {
    return a + ((b - a) * ArcadeKartClamp01(t));
}

static float ArcadeKartGetPlayerCVar(const char* prefix, int playerIndex, const char* suffix, float defaultValue) {
    char key[96];
    snprintf(key, sizeof(key), "%s.Player%d.%s", prefix, playerIndex + 1, suffix);
    return Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(key, defaultValue);
}

static int ArcadeKartGetPostFxViews(std::array<ArcadeKartPostFxView, 4>& views, const ImVec2& origin,
                                    const ImVec2& size) {
    const int screenMode =
        Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger("gArcadeKart.PostFx.ScreenMode", 0);

    views[0] = { 0, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), origin, ImVec2(origin.x + size.x, origin.y + size.y) };

    if (screenMode == 1) {
        views[0] = { 0, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 0.5f), origin,
                     ImVec2(origin.x + size.x, origin.y + (size.y * 0.5f)) };
        views[1] = { 1, ImVec2(0.0f, 0.5f), ImVec2(1.0f, 1.0f), ImVec2(origin.x, origin.y + (size.y * 0.5f)),
                     ImVec2(origin.x + size.x, origin.y + size.y) };
        return 2;
    }

    if (screenMode == 2) {
        views[0] = { 0, ImVec2(0.0f, 0.0f), ImVec2(0.5f, 1.0f), origin,
                     ImVec2(origin.x + (size.x * 0.5f), origin.y + size.y) };
        views[1] = { 1, ImVec2(0.5f, 0.0f), ImVec2(1.0f, 1.0f), ImVec2(origin.x + (size.x * 0.5f), origin.y),
                     ImVec2(origin.x + size.x, origin.y + size.y) };
        return 2;
    }

    if (screenMode == 3) {
        views[0] = { 0, ImVec2(0.0f, 0.0f), ImVec2(0.5f, 0.5f), origin,
                     ImVec2(origin.x + (size.x * 0.5f), origin.y + (size.y * 0.5f)) };
        views[1] = { 1, ImVec2(0.5f, 0.0f), ImVec2(1.0f, 0.5f), ImVec2(origin.x + (size.x * 0.5f), origin.y),
                     ImVec2(origin.x + size.x, origin.y + (size.y * 0.5f)) };
        views[2] = { 2, ImVec2(0.0f, 0.5f), ImVec2(0.5f, 1.0f), ImVec2(origin.x, origin.y + (size.y * 0.5f)),
                     ImVec2(origin.x + (size.x * 0.5f), origin.y + size.y) };
        views[3] = { 3, ImVec2(0.5f, 0.5f), ImVec2(1.0f, 1.0f),
                     ImVec2(origin.x + (size.x * 0.5f), origin.y + (size.y * 0.5f)),
                     ImVec2(origin.x + size.x, origin.y + size.y) };
        return 4;
    }

    return 1;
}

static ImVec2 ArcadeKartWarpPoint(const ArcadeKartPostFxView& view, float x, float y, float barrelStrength,
                                  float overscanPercent, float shakeX, float shakeY, float extraScale) {
    const ImVec2 center((view.minPos.x + view.maxPos.x) * 0.5f, (view.minPos.y + view.maxPos.y) * 0.5f);
    const float width = view.maxPos.x - view.minPos.x;
    const float height = view.maxPos.y - view.minPos.y;
    const float nx = (x * 2.0f) - 1.0f;
    const float ny = (y * 2.0f) - 1.0f;
    const float r2 = (nx * nx) + (ny * ny);
    const float warpScale = (1.0f + overscanPercent + extraScale) * (1.0f + (barrelStrength * r2));

    return ImVec2(center.x + ((nx * width * 0.5f) * warpScale) + shakeX,
                  center.y + ((ny * height * 0.5f) * warpScale) + shakeY);
}

static void ArcadeKartDrawWarpedView(ImDrawList* drawList, ImTextureID textureId, const ArcadeKartPostFxView& view,
                                     float barrelStrength, float overscanPercent, float shakeX, float shakeY,
                                     float extraScale, ImU32 tint) {
    constexpr int kGrid = 14;

    drawList->PushClipRect(view.minPos, view.maxPos, true);
    for (int y = 0; y < kGrid; y++) {
        const float y0 = static_cast<float>(y) / kGrid;
        const float y1 = static_cast<float>(y + 1) / kGrid;
        const float v0 = view.minUv.y + ((view.maxUv.y - view.minUv.y) * y0);
        const float v1 = view.minUv.y + ((view.maxUv.y - view.minUv.y) * y1);

        for (int x = 0; x < kGrid; x++) {
            const float x0 = static_cast<float>(x) / kGrid;
            const float x1 = static_cast<float>(x + 1) / kGrid;
            const float u0 = view.minUv.x + ((view.maxUv.x - view.minUv.x) * x0);
            const float u1 = view.minUv.x + ((view.maxUv.x - view.minUv.x) * x1);

            drawList->AddImageQuad(textureId, ArcadeKartWarpPoint(view, x0, y0, barrelStrength, overscanPercent, shakeX,
                                                                  shakeY, extraScale),
                                   ArcadeKartWarpPoint(view, x1, y0, barrelStrength, overscanPercent, shakeX, shakeY,
                                                       extraScale),
                                   ArcadeKartWarpPoint(view, x1, y1, barrelStrength, overscanPercent, shakeX, shakeY,
                                                       extraScale),
                                   ArcadeKartWarpPoint(view, x0, y1, barrelStrength, overscanPercent, shakeX, shakeY,
                                                       extraScale),
                                   ImVec2(u0, v0), ImVec2(u1, v0), ImVec2(u1, v1), ImVec2(u0, v1), tint);
        }
    }
    drawList->PopClipRect();
}

static void ArcadeKartDrawPostFxGame(ImTextureID textureId, const ImVec2& origin, const ImVec2& size) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();
    const float overscanPercent =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.OverscanPercent", 0.15f), 0.0f, 0.35f);
    const float baseBarrel = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.BarrelStrength", 0.035f), 0.0f, 0.35f);
    const float speedBarrel = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.SpeedBarrelStrength", 0.085f), 0.0f, 0.5f);
    const float boostBarrel = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.BoostBarrelStrength", 0.12f), 0.0f, 0.65f);
    const float warpIntensity = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.WarpIntensity", 1.0f), 0.0f, 3.0f);
    const float warpOutputScale =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.WarpOutputScale", 4.0f), 0.0f, 8.0f);
    const float motionBlur = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.MotionBlur", 0.28f), 0.0f, 1.0f);
    const float shakeStrength = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeStrength", 0.018f), 0.0f, 0.1f);
    const float shakeOutputScale =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeOutputScale", 0.3f), 0.0f, 2.0f);
    const float shakeFrequencyFastHz =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeFrequencyFastHz", 60.0f), 0.0f, 120.0f);
    const float shakeFrequencySlowHz =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeFrequencySlowHz", 8.0f), 0.0f, 120.0f);
    const float shakeFrequencyRoughnessPower =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeFrequencyRoughnessPower", 0.5f), 0.01f, 8.0f);
    const float speedMinRatio = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.SpeedMinRatio", 0.0f), 0.0f, 2.0f);
    const float speedMaxRatio = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.SpeedMaxRatio", 1.0f), 0.0f, 2.0f);
    const float responsePower = std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ResponsePower", 2.0f), 0.01f, 8.0f);
    const float shakeResponsePower =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.ShakeResponsePower", 3.0f), 0.01f, 8.0f);
    const bool tuningSlidersOnly = cvars->GetInteger("gArcadeKart.PostFx.TuningSlidersOnly", 0) != 0;
    const float testShakeSlider = ArcadeKartClamp01(cvars->GetFloat("gArcadeKart.PostFx.TestShakeSlider", 0.0f));
    const float testWarpSlider = ArcadeKartClamp01(cvars->GetFloat("gArcadeKart.PostFx.TestWarpSlider", 0.0f));
    const float tuningShakeInputMax =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.TuningShakeInputMax", 0.25f), 0.01f, 1.0f);
    const float tuningWarpInputMax =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.TuningWarpInputMax", 2.0f), 0.01f, 3.0f);
    const float tuningShakeStrength =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.TuningShakeStrength", 0.04f), 0.0f, 0.1f);
    const float tuningWarpStrength =
        std::clamp(cvars->GetFloat("gArcadeKart.PostFx.TuningWarpStrength", 0.16f), 0.0f, 1.0f);
    const bool manualOverride = cvars->GetInteger("gArcadeKart.PostFx.ManualOverride", 0) != 0;
    const float manualIntensity = ArcadeKartClamp01(cvars->GetFloat("gArcadeKart.PostFx.ManualIntensity", 0.0f));
    const double time = ImGui::GetTime();
    std::array<ArcadeKartPostFxView, 4> views;
    const int viewCount = ArcadeKartGetPostFxViews(views, origin, size);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    for (int i = 0; i < viewCount; i++) {
        const ArcadeKartPostFxView& view = views[i];
        const float speedRatio =
            manualOverride
                ? manualIntensity
                : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex, "SpeedRatio", 0.0f));
        const float boostAmount =
            manualOverride
                ? manualIntensity
                : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex, "BoostAmount", 0.0f));
        const float shakeAmount =
            manualOverride
                ? manualIntensity
                : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex, "ShakeAmount", 0.0f));
        const float roadRoughness =
            manualOverride ? 0.0f
                           : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex,
                                                                        "RoadRoughness", 0.0f));
        const float driftAmount =
            manualOverride ? 0.0f
                           : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex,
                                                                        "DriftAmount", 0.0f));
        const float boostShakeAmount =
            manualOverride ? 0.0f
                           : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex,
                                                                        "BoostShakeAmount", 0.0f));
        const float fxScale =
            manualOverride ? 1.0f
                           : ArcadeKartClamp01(ArcadeKartGetPlayerCVar("gArcadeKart.PostFx", view.playerIndex,
                                                                        "FxScale", 0.0f));
        const float normalizedSpeed = ArcadeKartNormalizeRange(speedRatio, speedMinRatio, speedMaxRatio);
        const float speedCurve = tuningSlidersOnly ? 0.0f : ArcadeKartApplyPostFxCurve(normalizedSpeed, responsePower);
        const float boostCurve = tuningSlidersOnly ? 0.0f : ArcadeKartApplyPostFxCurve(boostAmount, responsePower);
        const float roughnessShakeInput =
            ArcadeKartClamp01((normalizedSpeed * roadRoughness) + (driftAmount > 0.0f ? 0.1f : 0.0f) +
                              (boostShakeAmount > 0.0f ? 0.3f : 0.0f));
        const float roughnessCurve =
            tuningSlidersOnly ? 0.0f : ArcadeKartApplyPostFxCurve(roughnessShakeInput, shakeResponsePower);
        const float tunedShakeInput =
            ArcadeKartApplyPostFxCurve(testShakeSlider, shakeResponsePower) * tuningShakeInputMax;
        const float tunedWarpInput = ArcadeKartApplyPostFxCurve(testWarpSlider, responsePower) * tuningWarpInputMax;
        const float intensity =
            (tuningSlidersOnly ? ArcadeKartClamp01(tunedWarpInput / tuningWarpInputMax)
                               : ArcadeKartClamp01((speedCurve * 0.65f) + boostCurve)) *
            fxScale;
        const float barrelStrength =
            (tuningSlidersOnly
                 ? (tunedWarpInput * tuningWarpStrength)
                 : ((baseBarrel + (speedBarrel * speedCurve) + (boostBarrel * boostCurve)) * warpIntensity)) *
            fxScale * warpOutputScale;
        const float viewWidth = view.maxPos.x - view.minPos.x;
        const float viewHeight = view.maxPos.y - view.minPos.y;
        const float shakeImpulseCurve =
            tuningSlidersOnly ? 0.0f : ArcadeKartApplyPostFxCurve(shakeAmount, shakeResponsePower);
        const float shakeDriver = tuningSlidersOnly ? tunedShakeInput : std::max(roughnessCurve, shakeImpulseCurve);
        const float shakePixels =
            tuningSlidersOnly
                ? (std::min(viewWidth, viewHeight) * shakeDriver * tuningShakeStrength * fxScale)
                : (std::min(viewWidth, viewHeight) * shakeStrength * shakeDriver * fxScale);
        const float scaledShakePixels = shakePixels * shakeOutputScale;
        const float inverseRoughnessCurve =
            ArcadeKartApplyPostFxCurve(1.0f - roadRoughness, shakeFrequencyRoughnessPower);
        const float shakeFrequencyHz = ArcadeKartLerp(shakeFrequencySlowHz, shakeFrequencyFastHz, inverseRoughnessCurve);
        const float shakeAngularFrequency = shakeFrequencyHz * 6.28318530718f;
        const float shakeX =
            static_cast<float>(std::sin((time * shakeAngularFrequency) + (view.playerIndex * 1.7))) *
            scaledShakePixels;
        const float shakeY =
            static_cast<float>(std::cos((time * shakeAngularFrequency) + (view.playerIndex * 2.3))) *
            scaledShakePixels;
        if (view.playerIndex == 0) {
            cvars->SetFloat("gArcadeKart.PostFx.DebugActiveBarrel", barrelStrength);
            cvars->SetFloat("gArcadeKart.PostFx.DebugActiveIntensity", intensity);
            cvars->SetFloat("gArcadeKart.PostFx.DebugActiveShakePixels", scaledShakePixels);
            cvars->SetFloat("gArcadeKart.PostFx.DebugShakeFrequencyHz", shakeFrequencyHz);
            cvars->SetFloat("gArcadeKart.PostFx.DebugShakeFrequencyCurve", inverseRoughnessCurve);
            cvars->SetFloat("gArcadeKart.PostFx.DebugSpeedCurve", speedCurve);
            cvars->SetFloat("gArcadeKart.PostFx.DebugRoughnessCurve", roughnessCurve);
            cvars->SetFloat("gArcadeKart.PostFx.DebugRoughnessShakeInput", roughnessShakeInput);
        }
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        if (ArcadeKartShouldUseDx11PostFxShader()) {
            ArcadeKartScenePostFxShaderConstants constants = {};
            constants.uvMinX = view.minUv.x;
            constants.uvMinY = view.minUv.y;
            constants.uvSizeX = view.maxUv.x - view.minUv.x;
            constants.uvSizeY = view.maxUv.y - view.minUv.y;
            constants.barrelStrength = barrelStrength;
            constants.overscanPercent = overscanPercent;
            constants.blurStrength = std::clamp(motionBlur * intensity, 0.0f, 1.0f);
            constants.shakeUvX = constants.uvSizeX * (-shakeX / std::max(viewWidth, 1.0f));
            constants.shakeUvY = constants.uvSizeY * (-shakeY / std::max(viewHeight, 1.0f));
            ArcadeKartDrawDx11PostFxView(drawList, textureId, view, constants);
            continue;
        }
#endif
        const int blurAlpha = static_cast<int>(std::clamp(42.0f * motionBlur * intensity, 0.0f, 70.0f));

        if (blurAlpha > 0) {
            ArcadeKartDrawWarpedView(drawList, textureId, view, barrelStrength * 0.65f, overscanPercent, shakeX * 0.55f,
                                     shakeY * 0.55f, 0.018f + (0.030f * intensity),
                                     IM_COL32(255, 255, 255, blurAlpha));
            ArcadeKartDrawWarpedView(drawList, textureId, view, barrelStrength * 0.45f, overscanPercent, -shakeX * 0.35f,
                                     -shakeY * 0.35f, 0.032f + (0.050f * intensity),
                                     IM_COL32(255, 255, 255, blurAlpha / 2));
        }

        ArcadeKartDrawWarpedView(drawList, textureId, view, barrelStrength, overscanPercent, shakeX, shakeY, 0.0f,
                                 IM_COL32_WHITE);
    }
}

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
static std::array<ArcadeKartScenePostFxDrawData, 4> sArcadeKartScenePostFxDrawData;
static ID3D11Device* sArcadeKartDx11PostFxDevice = nullptr;
static ID3D11PixelShader* sArcadeKartScenePostFxPixelShader = nullptr;
static ID3D11PixelShader* sArcadeKartHudCompositePixelShader = nullptr;
static ID3D11Buffer* sArcadeKartScenePostFxConstantBuffer = nullptr;
static ID3D11BlendState* sArcadeKartHudCompositeBlendState = nullptr;
static ID3D11SamplerState* sArcadeKartHudCompositeSamplerState = nullptr;

template <typename T> static void ArcadeKartReleaseDx11Object(T*& object) {
    if (object != nullptr) {
        object->Release();
        object = nullptr;
    }
}

static void ArcadeKartResetDx11PostFxResources(ID3D11Device* device) {
    if (sArcadeKartDx11PostFxDevice == device) {
        return;
    }

    ArcadeKartReleaseDx11Object(sArcadeKartScenePostFxPixelShader);
    ArcadeKartReleaseDx11Object(sArcadeKartHudCompositePixelShader);
    ArcadeKartReleaseDx11Object(sArcadeKartScenePostFxConstantBuffer);
    ArcadeKartReleaseDx11Object(sArcadeKartHudCompositeBlendState);
    ArcadeKartReleaseDx11Object(sArcadeKartHudCompositeSamplerState);
    sArcadeKartDx11PostFxDevice = device;
}

static bool ArcadeKartShouldUseDx11PostFxShader() {
    return Ship::Context::GetRawInstance()->GetWindow()->GetWindowBackend() == Fast::WindowBackend::FAST3D_DXGI_DX11;
}

static bool ArcadeKartCreateDx11PixelShader(ID3D11Device* device, const char* shaderSource,
                                            ID3D11PixelShader** pixelShader) {
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;
    const UINT compileFlags =
#if defined(_DEBUG)
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        D3DCOMPILE_ENABLE_STRICTNESS;
#endif

    const HRESULT compileResult =
        D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "main", "ps_4_0", compileFlags, 0,
                   &shaderBlob, &errorBlob);

    if (errorBlob != nullptr) {
        errorBlob->Release();
    }

    if (FAILED(compileResult) || shaderBlob == nullptr) {
        return false;
    }

    const HRESULT shaderResult =
        device->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, pixelShader);
    shaderBlob->Release();
    return SUCCEEDED(shaderResult);
}

static bool ArcadeKartEnsureDx11PostFxResources(ImGui_ImplDX11_RenderState* renderState) {
    if (renderState == nullptr || renderState->Device == nullptr || renderState->DeviceContext == nullptr) {
        return false;
    }

    ArcadeKartResetDx11PostFxResources(renderState->Device);

    if (sArcadeKartScenePostFxPixelShader == nullptr) {
        static const char* kScenePostFxShader =
            "cbuffer ArcadeKartPostFxBuffer : register(b1) {"
            "  float4 uvRect;"
            "  float4 params;"
            "  float4 offsets;"
            "};"
            "struct PS_INPUT {"
            "  float4 pos : SV_POSITION;"
            "  float4 col : COLOR0;"
            "  float2 uv : TEXCOORD0;"
            "};"
            "sampler sampler0;"
            "Texture2D texture0;"
            "float2 ArcadeKartSampleUv(float2 sourceLocal) {"
            "  float2 uv = uvRect.xy + (saturate(sourceLocal) * uvRect.zw) + offsets.xy;"
            "  return clamp(uv, uvRect.xy, uvRect.xy + uvRect.zw);"
            "}"
            "float4 main(PS_INPUT input) : SV_Target {"
            "  float2 uvSize = max(uvRect.zw, float2(0.0001f, 0.0001f));"
            "  float2 local = saturate((input.uv - uvRect.xy) / uvSize);"
            "  float2 centered = (local * 2.0f) - 1.0f;"
            "  float r2 = dot(centered, centered);"
            "  float scale = max(1.0f + params.y + (params.x * r2), 0.001f);"
            "  float2 sourceLocal = ((centered / scale) * 0.5f) + 0.5f;"
            "  float blur = saturate(params.z);"
            "  float2 radialBlur = centered * blur * 0.018f;"
            "  float4 color = texture0.Sample(sampler0, ArcadeKartSampleUv(sourceLocal)) * 0.70f;"
            "  color += texture0.Sample(sampler0, ArcadeKartSampleUv(sourceLocal - radialBlur)) * 0.18f;"
            "  color += texture0.Sample(sampler0, ArcadeKartSampleUv(sourceLocal + radialBlur)) * 0.12f;"
            "  return float4(color.rgb * input.col.rgb, input.col.a);"
            "}";

        if (!ArcadeKartCreateDx11PixelShader(renderState->Device, kScenePostFxShader,
                                             &sArcadeKartScenePostFxPixelShader)) {
            return false;
        }
    }

    if (sArcadeKartHudCompositePixelShader == nullptr) {
        static const char* kHudCompositeShader =
            "struct PS_INPUT {"
            "  float4 pos : SV_POSITION;"
            "  float4 col : COLOR0;"
            "  float2 uv : TEXCOORD0;"
            "};"
            "sampler sampler0;"
            "Texture2D texture0;"
            "float4 main(PS_INPUT input) : SV_Target {"
            "  float4 texel = texture0.Sample(sampler0, input.uv);"
            "  float3 keyColor = float3(1.0f, 0.0f, 1.0f);"
            "  float keyDistance = distance(texel.rgb, keyColor);"
            "  float keyedAlpha = smoothstep(0.035f, 0.18f, keyDistance);"
            "  float3 color = texel.rgb;"
            "  float magentaBalance = saturate(1.0f - abs(color.r - color.b) * 18.0f);"
            "  float brightNeutralEdge = saturate((min(color.r, color.b) - 0.52f) * 4.0f) * magentaBalance;"
            "  float greenDeficit = saturate((min(color.r, color.b) - color.g) * 3.0f);"
            "  color.g = lerp(color.g, min(color.r, color.b), brightNeutralEdge * greenDeficit);"
            "  keyedAlpha *= saturate(texel.a * 64.0f);"
            "  return float4(color * input.col.rgb, keyedAlpha * input.col.a);"
            "}";

        if (!ArcadeKartCreateDx11PixelShader(renderState->Device, kHudCompositeShader,
                                             &sArcadeKartHudCompositePixelShader)) {
            return false;
        }
    }

    if (sArcadeKartScenePostFxConstantBuffer == nullptr) {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = sizeof(ArcadeKartScenePostFxShaderConstants);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        if (renderState->Device->CreateBuffer(&bufferDesc, nullptr, &sArcadeKartScenePostFxConstantBuffer) != S_OK) {
            return false;
        }
    }

    if (sArcadeKartHudCompositeBlendState == nullptr) {
        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = true;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        if (renderState->Device->CreateBlendState(&blendDesc, &sArcadeKartHudCompositeBlendState) != S_OK) {
            return false;
        }
    }

    if (sArcadeKartHudCompositeSamplerState == nullptr) {
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        if (renderState->Device->CreateSamplerState(&samplerDesc, &sArcadeKartHudCompositeSamplerState) != S_OK) {
            return false;
        }
    }

    return true;
}

static void ArcadeKartSetScenePostFxShader(const ImDrawList*, const ImDrawCmd* cmd) {
    auto* renderState = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    const ArcadeKartScenePostFxDrawData* drawData =
        static_cast<const ArcadeKartScenePostFxDrawData*>(cmd->UserCallbackData);

    if (drawData == nullptr || !ArcadeKartEnsureDx11PostFxResources(renderState)) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (renderState->DeviceContext->Map(sArcadeKartScenePostFxConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
                                        &mapped) == S_OK) {
        memcpy(mapped.pData, &drawData->constants, sizeof(drawData->constants));
        renderState->DeviceContext->Unmap(sArcadeKartScenePostFxConstantBuffer, 0);
    }

    renderState->DeviceContext->PSSetShader(sArcadeKartScenePostFxPixelShader, nullptr, 0);
    renderState->DeviceContext->PSSetConstantBuffers(1, 1, &sArcadeKartScenePostFxConstantBuffer);
}

static void ArcadeKartSetHudCompositeShader(const ImDrawList*, const ImDrawCmd*) {
    auto* renderState = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);

    if (!ArcadeKartEnsureDx11PostFxResources(renderState)) {
        return;
    }

    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    renderState->DeviceContext->PSSetShader(sArcadeKartHudCompositePixelShader, nullptr, 0);
    renderState->DeviceContext->PSSetSamplers(0, 1, &sArcadeKartHudCompositeSamplerState);
    renderState->DeviceContext->OMSetBlendState(sArcadeKartHudCompositeBlendState, blendFactor, 0xFFFFFFFF);
}

static void ArcadeKartDrawDx11PostFxView(ImDrawList* drawList, ImTextureID textureId,
                                         const ArcadeKartPostFxView& view,
                                         const ArcadeKartScenePostFxShaderConstants& constants) {
    ArcadeKartScenePostFxDrawData& drawData =
        sArcadeKartScenePostFxDrawData[std::clamp(view.playerIndex, 0, 3)];

    drawData.constants = constants;
    drawList->PushClipRect(view.minPos, view.maxPos, true);
    drawList->AddCallback(ArcadeKartSetScenePostFxShader, &drawData);
    drawList->AddImage(textureId, view.minPos, view.maxPos, view.minUv, view.maxUv, IM_COL32_WHITE);
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    drawList->PopClipRect();
}
#endif

static void ArcadeKartDrawHudLayer(ImTextureID textureId, const ImVec2& origin, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 maxPos(origin.x + size.x, origin.y + size.y);
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();

#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
    if (ArcadeKartShouldUseDx11PostFxShader()) {
        if (cvars->GetInteger("gArcadeKart.PostFx.DebugHudAlphaBlend", 0) != 0) {
            drawList->AddImage(textureId, origin, maxPos);
            return;
        }

        std::array<ArcadeKartPostFxView, 4> views;
        const int viewCount = ArcadeKartGetPostFxViews(views, origin, size);

        drawList->AddCallback(ArcadeKartSetHudCompositeShader, nullptr);
        for (int i = 0; i < viewCount; i++) {
            drawList->PushClipRect(views[i].minPos, views[i].maxPos, true);
            drawList->AddImage(textureId, views[i].minPos, views[i].maxPos, views[i].minUv, views[i].maxUv);
            drawList->PopClipRect();
        }
        drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        return;
    }
#endif

    drawList->AddImage(textureId, origin, maxPos);
}

static bool ArcadeKartDrawLayeredPostFxGame(const ImVec2& origin, const ImVec2& size) {
    auto cvars = Ship::Context::GetRawInstance()->GetConsoleVariables();

    if (cvars->GetInteger("gArcadeKart.PostFx.LayerHud", 1) == 0 ||
        cvars->GetInteger("gArcadeKart.PostFx.LayeredHudActive", 0) == 0) {
        return false;
    }

    const int sceneFramebufferId = cvars->GetInteger("gArcadeKart.PostFx.SceneFramebufferId", -1);
    const int hudFramebufferId = cvars->GetInteger("gArcadeKart.PostFx.HudFramebufferId", -1);
    const uintptr_t sceneFramebuffer = gfx_get_framebuffer_texture_id(sceneFramebufferId);
    const uintptr_t hudFramebuffer = gfx_get_framebuffer_texture_id(hudFramebufferId);

    if (sceneFramebuffer == 0 || hudFramebuffer == 0) {
        cvars->SetInteger("gArcadeKart.PostFx.LayeredHudActive", 0);
        return false;
    }

    ArcadeKartDrawPostFxGame(reinterpret_cast<ImTextureID>(sceneFramebuffer), origin, size);
    ArcadeKartDrawHudLayer(reinterpret_cast<ImTextureID>(hudFramebuffer), origin, size);
    cvars->SetInteger("gArcadeKart.PostFx.LayeredHudActive", 0);
    return true;
}
} // namespace

namespace Fast {

Fast3dGui::Fast3dGui() : Ship::Gui() {
}

Fast3dGui::Fast3dGui(std::vector<std::shared_ptr<Ship::GuiWindow>> guiWindows) : Ship::Gui(guiWindows) {
}

void Fast3dGui::Init(GuiWindowInitData windowImpl) {
    mImpl = windowImpl;
    Gui::Init();
}

bool Fast3dGui::SupportsViewports() {
#ifdef __linux__
    const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (currentDesktop && std::string(currentDesktop) == "gamescope") {
        return false;
    }
#endif

#if defined(__ANDROID__) || defined(__IOS__)
    return false;
#endif

    return true;
}

void Fast3dGui::HandleWindowEvents(Fast::WindowEvent event) {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL:
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplSDL2_ProcessEvent(static_cast<const SDL_Event*>(event.Sdl.Event));
#if defined(__ANDROID__) || defined(__IOS__)
            Ship::Mobile::ImGuiProcessEvent(ImGui::GetIO().WantTextInput);
#endif
            break;
#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(event.Win32.Handle), event.Win32.Msg, event.Win32.Param1,
                                           event.Win32.Param2);
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiWMInit() {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL:
            SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
            if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1)) {
                SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            }
            ImGui_ImplSDL2_InitForOpenGL(static_cast<SDL_Window*>(mImpl.Opengl.Window), mImpl.Opengl.Context);
            break;
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
            if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1)) {
                SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            }
            ImGui_ImplSDL2_InitForMetal(static_cast<SDL_Window*>(mImpl.Metal.Window));
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_Init(mImpl.Dx11.Window);
            break;
#endif
        default:
            break;
    }

    // Initial gamepad bind once the SDL2 backend exists (no-op for DX/Metal).
    RefreshImGuiGamepads();
}

void Fast3dGui::ImGuiWMShutdown() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplSDL2_Shutdown();
            break;
#endif
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplSDL2_Shutdown();
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_Shutdown();
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendInit() {
    auto window = Ship::Context::GetRawInstance()->GetWindow();
    mInterpreter = std::dynamic_pointer_cast<Fast3dWindow>(window)->GetInterpreterWeak();
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
#ifdef __APPLE__
            ImGui_ImplOpenGL3_Init("#version 410 core");
#elif USE_OPENGLES
            ImGui_ImplOpenGL3_Init("#version 300 es");
#else
            ImGui_ImplOpenGL3_Init("#version 120");
#endif
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL: {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->MetalInit(mImpl.Metal.Renderer);
            break;
        }
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_Init(static_cast<ID3D11Device*>(mImpl.Dx11.Device),
                                static_cast<ID3D11DeviceContext*>(mImpl.Dx11.DeviceContext));
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendShutdown() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_Shutdown();
            break;
#endif
#if __APPLE__
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplMetal_Shutdown();
            break;
#endif
#if defined(ENABLE_DX11) || defined(ENABLE_DX12)
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_Shutdown();
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiBackendNewFrame() {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_NewFrame();
            break;
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_NewFrame();
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL: {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->NewFrame();
            break;
        }
#endif
        default:
            break;
    }
}

void Fast3dGui::ImGuiWMNewFrame() {
    switch (mImpl.Backend) {
        case WindowBackend::FAST3D_SDL_OPENGL:
        case WindowBackend::FAST3D_SDL_METAL:
            ImGui_ImplSDL2_NewFrame();
            break;
#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplWin32_NewFrame();
            break;
#endif
        default:
            break;
    }
}

// Bind ImGui's SDL2 gamepad backend to the controller(s) the
// ControlDeck has already opened
void Fast3dGui::RefreshImGuiGamepads() {
    if (mImpl.Backend != WindowBackend::FAST3D_SDL_OPENGL && mImpl.Backend != WindowBackend::FAST3D_SDL_METAL) {
        return;
    }

    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll, nullptr, 0);
}

void Fast3dGui::ImGuiRenderDrawData(ImDrawData* data) {
    switch (mImpl.Backend) {
#ifdef ENABLE_OPENGL
        case WindowBackend::FAST3D_SDL_OPENGL:
            ImGui_ImplOpenGL3_RenderDrawData(data);
            break;
#endif

#ifdef __APPLE__
        case WindowBackend::FAST3D_SDL_METAL: {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->RenderDrawData(data);
            break;
        }
#endif

#ifdef ENABLE_DX11
        case WindowBackend::FAST3D_DXGI_DX11:
            ImGui_ImplDX11_RenderDrawData(data);
            break;
#endif
        default:
            break;
    }
}

void Fast3dGui::DrawFloatingWindows() {
    if (!(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        return;
    }

    // OpenGL requires extra platform handling for the GL context
    if (mImpl.Backend == WindowBackend::FAST3D_SDL_OPENGL && mImpl.Opengl.Context != nullptr) {
        // Backup window and context before calling RenderPlatformWindowsDefault
        SDL_Window* backupCurrentWindow = SDL_GL_GetCurrentWindow();
        SDL_GLContext backupCurrentContext = SDL_GL_GetCurrentContext();

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();

        // Restore GL context for next frame
        SDL_GL_MakeCurrent(backupCurrentWindow, backupCurrentContext);
    } else {
#ifdef __APPLE__
        // Metal requires additional frame setup to get ImGui ready for drawing floating windows
        if (mImpl.Backend == WindowBackend::FAST3D_SDL_METAL) {
            GfxRenderingAPIMetal* api = (GfxRenderingAPIMetal*)mInterpreter.lock()->GetCurrentRenderingAPI();
            api->SetupFloatingFrame();
        }
#endif

        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void Fast3dGui::CalculateGameViewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Main Game", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    ImVec2 mainPos = ImGui::GetWindowPos();
    mainPos.x -= mTemporaryWindowPos.x;
    mainPos.y -= mTemporaryWindowPos.y;
    ImVec2 size = ImGui::GetContentRegionAvail();
    const auto interpreter = mInterpreter.lock().get();
    interpreter->mCurDimensions.width = (uint32_t)(size.x * mInterpreter.lock()->mCurDimensions.internal_mul);
    interpreter->mCurDimensions.height = (uint32_t)(size.y * mInterpreter.lock()->mCurDimensions.internal_mul);
    interpreter->mGameWindowViewport.x = (int16_t)mainPos.x;
    interpreter->mGameWindowViewport.y = (int16_t)mainPos.y;
    interpreter->mGameWindowViewport.width = (int16_t)size.x;
    interpreter->mGameWindowViewport.height = (int16_t)size.y;

    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled",
                                                                           0)) {
        ApplyResolutionChanges();
    }

    switch (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_LOW_RES_MODE, 0)) {
        case 1: { // N64 Mode
            interpreter->mCurDimensions.width = 320;
            interpreter->mCurDimensions.height = 240;
            /*
            const int sw = size.y * 320 / 240;
            mInterpreter.lock()->mGameWindowViewport.x += ((int)size.x - sw) / 2;
            mInterpreter.lock()->mGameWindowViewport.width = sw;*/
            break;
        }
        case 2: { // 240p Widescreen
            constexpr int vertRes = 240;
            interpreter->mCurDimensions.width = vertRes * size.x / size.y;
            interpreter->mCurDimensions.height = vertRes;
            break;
        }
        case 3: { // 480p Widescreen
            constexpr int vertRes = 480;
            interpreter->mCurDimensions.width = vertRes * size.x / size.y;
            interpreter->mCurDimensions.height = vertRes;
            break;
        }
    }

    ImGui::End();
}

void Fast3dGui::DrawGame() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground;

    ImGui::Begin("Main Game", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    GetGameOverlay()->Draw();

    ImVec2 mainPos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImVec2 pos = ImVec2(0, 0);
    const auto interpreter = mInterpreter.lock().get();

    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_LOW_RES_MODE, 0) ==
        1) { // N64 Mode takes priority
        const float sw = size.y * 320.0f / 240.0f;
        pos = ImVec2(floor(size.x / 2 - sw / 2), 0);
        size = ImVec2(sw, size.y);
    } else if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                   CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0)) {
        if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                CVAR_PREFIX_ADVANCED_RESOLUTION ".PixelPerfectMode", 0)) {
            if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                    CVAR_PREFIX_ADVANCED_RESOLUTION ".IgnoreAspectCorrection", 0)) {
                float sWdth = size.y * interpreter->mCurDimensions.width / interpreter->mCurDimensions.height;
                float sHght = size.x * interpreter->mCurDimensions.height / interpreter->mCurDimensions.width;

                float sPosX = floor(size.x / 2.0f - sWdth / 2.0f);
                float sPosY = floor(size.y / 2.0f - sHght / 2.0f);
                if (sPosY < 0.0f) { // pillarbox
                    sPosY = 0.0f;   // clamp y position
                    sHght = size.y; // reset height
                }
                if (sPosX < 0.0f) { // letterbox
                    sPosX = 0.0f;   // clamp x position
                    sWdth = size.x; // reset width
                }
                pos = ImVec2(sPosX, sPosY);
                size = ImVec2(sWdth, sHght);
            }
        } else { // in pixel perfect mode it's much easier
            const int factor = GetIntegerScaleFactor();
            float sPosX = floor(size.x / 2.0f - (interpreter->mCurDimensions.width * factor) / 2.0f);
            float sPosY = floor(size.y / 2.0f - (interpreter->mCurDimensions.height * factor) / 2.0f);
            pos = ImVec2(sPosX, sPosY);
            size = ImVec2(float(interpreter->mCurDimensions.width) * factor,
                          float(interpreter->mCurDimensions.height) * factor);
        }
    }
    uintptr_t fb = Ship::Context::GetRawInstance()->GetWindow()->GetGfxFrameBuffer();
    ImGui::SetCursorPos(pos);
    ImVec2 imagePos = ImGui::GetCursorScreenPos();
    if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger("gArcadeKart.PostFx.Enabled", 1) != 0) {
        if (ArcadeKartDrawLayeredPostFxGame(imagePos, size)) {
            ImGui::Dummy(size);
        } else if (fb) {
            ArcadeKartDrawPostFxGame(reinterpret_cast<ImTextureID>(fb), imagePos, size);
            ImGui::Dummy(size);
        }
    } else if (fb) {
        ImGui::Image(reinterpret_cast<ImTextureID>(fb), size);
    }
    DrawGameImageOverlay(imagePos, size);
    ImGui::End();
}

void Fast3dGui::ApplyResolutionChanges() {
    ImVec2 size = ImGui::GetContentRegionAvail();

    const float aspectRatioX = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".AspectRatioX", 16.0f);
    const float aspectRatioY = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetFloat(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".AspectRatioY", 9.0f);
    const uint32_t verticalPixelCount = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalPixelCount", 480);
    const bool verticalResolutionToggle = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);

    const bool aspectRatioIsEnabled = (aspectRatioX > 0.0f) && (aspectRatioY > 0.0f);

    constexpr uint32_t minResolutionWidth = 320;
    constexpr uint32_t minResolutionHeight = 240;
    constexpr uint32_t maxResolutionWidth = 8096;  // the renderer's actual limit is 16384
    constexpr uint32_t maxResolutionHeight = 4320; // on either axis. if you have the VRAM for it.
    uint32_t newWidth;
    uint32_t newHeight;
    const auto interpreter = mInterpreter.lock().get();
    interpreter->GetCurDimensions(&newWidth, &newHeight);

    if (verticalResolutionToggle) { // Use fixed vertical resolution
        if (aspectRatioIsEnabled) {
            newWidth = uint32_t(float(verticalPixelCount / aspectRatioY) * aspectRatioX);
        } else {
            newWidth = uint32_t(float(verticalPixelCount * size.x / size.y));
        }
        newHeight = verticalPixelCount;
    } else { // Use the window's resolution
        if (aspectRatioIsEnabled) {
            if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
                (aspectRatioY / aspectRatioX)) {
                // when pillarboxed
                newWidth = uint32_t(float(interpreter->mCurDimensions.height / aspectRatioY) * aspectRatioX);
            } else { // when letterboxed
                newHeight = uint32_t(float(interpreter->mCurDimensions.width / aspectRatioX) * aspectRatioY);
            }
        } // else, having both options turned off does nothing.
    }
    // clamp values to prevent renderer crash
    if (newWidth < minResolutionWidth) {
        newWidth = minResolutionWidth;
    }
    if (newHeight < minResolutionHeight) {
        newHeight = minResolutionHeight;
    }
    if (newWidth > maxResolutionWidth) {
        newWidth = maxResolutionWidth;
    }
    if (newHeight > maxResolutionHeight) {
        newHeight = maxResolutionHeight;
    }
    // apply new dimensions
    interpreter->mCurDimensions.width = newWidth;
    interpreter->mCurDimensions.height = newHeight;
    // centring the image is done in Fast3dGui::DrawGame().
}

int16_t Fast3dGui::GetIntegerScaleFactor() {
    const auto interpreter = mInterpreter.lock().get();
    if (!Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.FitAutomatically", 0)) {
        int16_t factor = Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.Factor", 1);

        if (Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
                CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.NeverExceedBounds", 1)) {
            if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
                ((float)interpreter->mCurDimensions.height / interpreter->mCurDimensions.width)) {
                if ((uint32_t)factor > interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height) {
                    factor = interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height;
                }
            } else {
                if ((uint32_t)factor > interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width) {
                    factor = interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width;
                }
            }
        }

        if (factor < 1) {
            factor = 1;
        }
        return factor;
    } else {
        int16_t factor = 1;

        if (((float)interpreter->mGameWindowViewport.height / interpreter->mGameWindowViewport.width) <
            ((float)interpreter->mCurDimensions.height / interpreter->mCurDimensions.width)) {
            factor = interpreter->mGameWindowViewport.height / interpreter->mCurDimensions.height;
        } else {
            factor = interpreter->mGameWindowViewport.width / interpreter->mCurDimensions.width;
        }

        factor += Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(
            CVAR_PREFIX_ADVANCED_RESOLUTION ".IntegerScale.ExceedBoundsBy", 0);

        if (factor < 1) {
            factor = 1;
        }
        return factor;
    }
}

ImTextureID Fast3dGui::GetTextureById(int32_t id) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
    return api->GetTextureById(id);
}

bool Fast3dGui::HasTextureByName(const std::string& name) {
    return mGuiTextures.contains(name);
}

ImTextureID Fast3dGui::GetTextureByName(const std::string& name) {
    if (!HasTextureByName(name)) {
        return nullptr;
    }
    return GetTextureById(mGuiTextures[name].RendererTextureId);
}

ImVec2 Fast3dGui::GetTextureSize(const std::string& name) {
    if (!HasTextureByName(name)) {
        return ImVec2(0, 0);
    }
    return ImVec2(mGuiTextures[name].Width, mGuiTextures[name].Height);
}

void Fast3dGui::LoadTextureFromRawImage(const std::string& name, const std::string& path) {
    auto initData = std::make_shared<Ship::ResourceInitData>();
    initData->Format = RESOURCE_FORMAT_BINARY;
    initData->Type = static_cast<uint32_t>(RESOURCE_TYPE_GUI_TEXTURE);
    initData->ResourceVersion = 0;
    initData->Path = path;
    auto guiTexture = std::static_pointer_cast<Ship::GuiTexture>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path, false, initData));

    LoadTextureFromResource(name, guiTexture);
}

void Fast3dGui::LoadTextureFromResource(const std::string& name, std::shared_ptr<Ship::GuiTexture> texture) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();

    // TODO: Nothing ever unloads the texture from Fast3D here.
    texture->Metadata.RendererTextureId = api->NewTexture();
    api->SelectTexture(0, texture->Metadata.RendererTextureId);
    api->SetSamplerParameters(0, false, 0, 0);
    api->UploadTexture(texture->Data, texture->Metadata.Width, texture->Metadata.Height);

    mGuiTextures[name] = texture->Metadata;
}

void Fast3dGui::LoadGuiTexture(const std::string& name, const Fast::Texture& res, const std::string& palettePath,
                               const ImVec4& tint) {
    GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
    std::vector<uint8_t> texBuffer;
    texBuffer.reserve(res.Width * res.Height * 4);

    // For HD textures we need to load the buffer raw (similar to inside gfx_pp)
    if ((res.Flags & TEX_FLAG_LOAD_AS_RAW) != 0) {
        // Raw loading doesn't support TLUT textures
        if (res.Type == Fast::TextureType::Palette8bpp) {
            // TODO convert other image types
            SPDLOG_WARN("ImGui::ResourceLoad: Attempting to load unsupported image type");
            return;
        }

        texBuffer.assign(res.ImageData, res.ImageData + (res.Width * res.Height * 4));
    } else {
        switch (res.Type) {
            case Fast::TextureType::RGBA32bpp:
                texBuffer.assign(res.ImageData, res.ImageData + (res.Width * res.Height * 4));
                break;
            case Fast::TextureType::RGBA16bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t b1 = res.ImageData[i * 2 + 0];
                    uint8_t b2 = res.ImageData[i * 2 + 1];
                    uint8_t r = (b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g = (((b1 & 7) << 2) | (b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b = ((b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a = 0xFF * (b2 & 1);
                    texBuffer.push_back(r);
                    texBuffer.push_back(g);
                    texBuffer.push_back(b);
                    texBuffer.push_back(a);
                }
                break;
            }
            case Fast::TextureType::Palette4bpp: {
                if (palettePath.empty()) {
                    SPDLOG_WARN("ImGui::ResourceLoad: Palette4bpp requires a palette path for asset: {}", name);
                    return;
                }

                auto paletteRes = std::static_pointer_cast<Fast::Texture>(
                    Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(palettePath));

                if (!paletteRes || !paletteRes->ImageData) {
                    SPDLOG_WARN("ImGui::ResourceLoad: Failed to load palette asset from path: {}", palettePath);
                    return;
                }

                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t idx1 = b >> 4;
                    uint8_t p1_b1 = paletteRes->ImageData[idx1 * 2 + 0];
                    uint8_t p1_b2 = paletteRes->ImageData[idx1 * 2 + 1];
                    uint8_t r1 = (p1_b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g1 = (((p1_b1 & 7) << 2) | (p1_b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b1 = ((p1_b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a1 = 0xFF * (p1_b2 & 1);
                    texBuffer.push_back(r1);
                    texBuffer.push_back(g1);
                    texBuffer.push_back(b1);
                    texBuffer.push_back(a1);

                    uint8_t idx2 = b & 0xF;
                    uint8_t p2_b1 = paletteRes->ImageData[idx2 * 2 + 0];
                    uint8_t p2_b2 = paletteRes->ImageData[idx2 * 2 + 1];
                    uint8_t r2 = (p2_b1 >> 3) * 0xFF / 0x1F;
                    uint8_t g2 = (((p2_b1 & 7) << 2) | (p2_b2 >> 6)) * 0xFF / 0x1F;
                    uint8_t b2 = ((p2_b2 >> 1) & 0x1F) * 0xFF / 0x1F;
                    uint8_t a2 = 0xFF * (p2_b2 & 1);
                    texBuffer.push_back(r2);
                    texBuffer.push_back(g2);
                    texBuffer.push_back(b2);
                    texBuffer.push_back(a2);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha16bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t color = res.ImageData[i * 2 + 0];
                    uint8_t alpha = res.ImageData[i * 2 + 1];
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha8bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t ia = res.ImageData[i];
                    uint8_t color = ((ia >> 4) & 0xF) * 255 / 15;
                    uint8_t alpha = (ia & 0xF) * 255 / 15;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::GrayscaleAlpha4bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t ia4 = b >> 4;
                    uint8_t color = ((ia4 >> 1) & 0xF) * 255 / 0b111;
                    uint8_t alpha = (ia4 & 1) * 255;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);

                    ia4 = b & 0xF;
                    color = ((ia4 >> 1) & 0xF) * 255 / 0b111;
                    alpha = (ia4 & 1) * 255;
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(color);
                    texBuffer.push_back(alpha);
                }
                break;
            }
            case Fast::TextureType::Grayscale8bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i++) {
                    uint8_t ia = res.ImageData[i];
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                    texBuffer.push_back(ia);
                }
                break;
            }
            case Fast::TextureType::Grayscale4bpp: {
                for (int32_t i = 0; i < res.Width * res.Height; i += 2) {
                    uint8_t b = res.ImageData[i / 2];

                    uint8_t ia4 = ((b >> 4) * 0xFF) / 0b1111;
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);

                    ia4 = ((b & 0xF) * 0xFF) / 0b1111;
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                    texBuffer.push_back(ia4);
                }
                break;
            }
            default:
                // TODO convert other image types
                SPDLOG_WARN("ImGui::ResourceLoad: Attempting to load unsupported image type");
                return;
        }
    }

    for (size_t pixel = 0; pixel < texBuffer.size() / 4; pixel++) {
        texBuffer[pixel * 4 + 0] *= tint.x;
        texBuffer[pixel * 4 + 1] *= tint.y;
        texBuffer[pixel * 4 + 2] *= tint.z;
        texBuffer[pixel * 4 + 3] *= tint.w;
    }

    Ship::GuiTextureMetadata asset;
    asset.RendererTextureId = api->NewTexture();
    asset.Width = res.Width;
    asset.Height = res.Height;

    api->SelectTexture(0, asset.RendererTextureId);
    api->SetSamplerParameters(0, false, 0, 0);
    api->UploadTexture(texBuffer.data(), res.Width, res.Height);

    mGuiTextures[name] = asset;
}

void Fast3dGui::LoadGuiTexture(const std::string& name, const std::string& path, const std::string& palettePath,
                               const ImVec4& tint) {
    const auto res = static_cast<Fast::Texture*>(
        Ship::Context::GetRawInstance()->GetResourceManager()->LoadResource(path, true).get());

    LoadGuiTexture(name, *res, palettePath, tint);
}

void Fast3dGui::UnloadTexture(const std::string& name) {
    if (mGuiTextures.contains(name)) {
        Ship::GuiTextureMetadata tex = mGuiTextures[name];
        GfxRenderingAPI* api = mInterpreter.lock()->GetCurrentRenderingAPI();
        api->DeleteTexture(tex.RendererTextureId);
        mGuiTextures.erase(name);
    }
}

} // namespace Fast
