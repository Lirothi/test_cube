#include "ocean/OceanControlsWindow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/Systems.h"
#include "core/math/Math.h"
#include "imgui.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "ocean/OceanSpectrum.h"
#include "rendering/core/Renderer.h"

namespace
{
    struct ResolutionOption
    {
        OceanSimulationSettings::ResolutionValue value;
        const char* label;
    };

    constexpr ResolutionOption kResolutionOptions[] = {
        { OceanSimulationSettings::ResolutionValue::Six, "64" },
        { OceanSimulationSettings::ResolutionValue::Seven, "128" },
        { OceanSimulationSettings::ResolutionValue::Eight, "256" },
        { OceanSimulationSettings::ResolutionValue::Nine, "512" },
    };

    struct CascadeCountOption
    {
        OceanSimulationSettings::CascadesNumberValue value;
        const char* label;
    };

    constexpr CascadeCountOption kCascadeCountOptions[] = {
        { OceanSimulationSettings::CascadesNumberValue::Two, "2" },
        { OceanSimulationSettings::CascadesNumberValue::Three, "3" },
        { OceanSimulationSettings::CascadesNumberValue::Four, "4" },
    };

    struct DomainModeOption
    {
        OceanSimulationSettings::CascadeDomainsMode value;
        const char* label;
    };

    constexpr DomainModeOption kDomainModeOptions[] = {
        { OceanSimulationSettings::CascadeDomainsMode::Auto, "Auto" },
        { OceanSimulationSettings::CascadeDomainsMode::Manual, "Manual" },
    };

    struct InputModeOption
    {
        OceanSimulationInputsProvider::InputsProviderMode value;
        const char* label;
    };

    constexpr InputModeOption kInputModeOptions[] = {
        { OceanSimulationInputsProvider::InputsProviderMode::Scale, "Wind scale" },
        { OceanSimulationInputsProvider::InputsProviderMode::Fixed, "Fixed" },
    };

    struct ReadbackModeOption
    {
        OceanSimulationSettings::ReadbackCascadesMode value;
        const char* label;
    };

    constexpr ReadbackModeOption kReadbackModeOptions[] = {
        { OceanSimulationSettings::ReadbackCascadesMode::None, "None" },
        { OceanSimulationSettings::ReadbackCascadesMode::One, "One" },
        { OceanSimulationSettings::ReadbackCascadesMode::Two, "Two" },
    };

    struct FilterTypeOption
    {
        EqualizerPreset::FilterType value;
        const char* label;
    };

    constexpr FilterTypeOption kFilterTypeOptions[] = {
        { EqualizerPreset::FilterType::Bell, "Bell" },
        { EqualizerPreset::FilterType::Highshelf, "High shelf" },
        { EqualizerPreset::FilterType::Lowshelf, "Low shelf" },
    };

    constexpr float kOceanControlItemWidth = 220.0f;
    constexpr float kOceanControlLabelMinWidth = 220.0f;
    constexpr float kOceanControlInputWidthFraction = 0.55f;
    constexpr int kEqualizerGraphSamples = 128;
    constexpr float kEqualizerGraphHeight = 150.0f;
    constexpr float kEqualizerGraphLabelHeight = 20.0f;
    constexpr int kSpectrumPlotSamples = 160;
    constexpr float kSpectrumPlotHeight = 200.0f;
    constexpr float kSpectrumPlotLabelHeight = 26.0f;

    constexpr const char* kSpectrumModelLabels[] = {
        "Pierson-Moskowitz",
        "JONSWAP",
        "TMA",
    };

    const char* ResolutionLabel(OceanSimulationSettings::ResolutionValue value)
    {
        for (const ResolutionOption& option : kResolutionOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    float CalculateOceanControlItemWidth()
    {
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float minimumCombinedWidth = kOceanControlItemWidth + kOceanControlLabelMinWidth;
        if (availableWidth <= minimumCombinedWidth)
        {
            return kOceanControlItemWidth;
        }

        const float desiredInputWidth = availableWidth * kOceanControlInputWidthFraction;
        const float maxInputWidthWithLabelRoom = availableWidth - kOceanControlLabelMinWidth;
        return std::clamp(desiredInputWidth, kOceanControlItemWidth, maxInputWidthWithLabelRoom);
    }

    const char* CascadeCountLabel(OceanSimulationSettings::CascadesNumberValue value)
    {
        for (const CascadeCountOption& option : kCascadeCountOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    const char* DomainModeLabel(OceanSimulationSettings::CascadeDomainsMode value)
    {
        for (const DomainModeOption& option : kDomainModeOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    const char* InputModeLabel(OceanSimulationInputsProvider::InputsProviderMode value)
    {
        for (const InputModeOption& option : kInputModeOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    const char* ReadbackModeLabel(OceanSimulationSettings::ReadbackCascadesMode value)
    {
        for (const ReadbackModeOption& option : kReadbackModeOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    const char* FilterTypeLabel(EqualizerPreset::FilterType value)
    {
        for (const FilterTypeOption& option : kFilterTypeOptions)
        {
            if (option.value == value)
            {
                return option.label;
            }
        }
        return "Unknown";
    }

    std::string Narrow(const std::wstring& value)
    {
        std::string out;
        out.reserve(value.size());
        for (wchar_t c : value)
        {
            out.push_back(c >= 0 && c <= 0x7f ? static_cast<char>(c) : '?');
        }
        return out;
    }

    std::wstring Widen(std::string_view value)
    {
        std::wstring out;
        out.reserve(value.size());
        for (char c : value)
        {
            out.push_back(static_cast<unsigned char>(c));
        }
        return out;
    }

    std::string FileNameLabel(const std::wstring& path)
    {
        return Narrow(std::filesystem::path(path).filename().wstring());
    }

    std::string FormatWavelength(float meters)
    {
        char label[32];
        if (meters < 1.0f)
        {
            std::snprintf(label, sizeof(label), "%.0f cm", meters * 100.0f);
        }
        else if (meters < 1000.0f)
        {
            std::snprintf(label, sizeof(label), "%.1f m", meters);
        }
        else
        {
            std::snprintf(label, sizeof(label), "%.1f km", meters / 1000.0f);
        }
        return label;
    }

    std::wstring BuildConfigPathFromName(std::string_view name)
    {
        std::filesystem::path fileName = std::filesystem::path(Widen(name)).filename();
        if (fileName.empty())
        {
            return {};
        }
        if (fileName.extension().empty())
        {
            fileName += L".json";
        }
        return (std::filesystem::path(L"data/ocean") / fileName).wstring();
    }

    std::shared_ptr<EqualizerPreset> CloneEqualizerPreset(const std::shared_ptr<EqualizerPreset>& source)
    {
        const std::shared_ptr<EqualizerPreset> safe = source ? source : EqualizerPreset::CreateDefault();
        return std::make_shared<EqualizerPreset>(safe->GetScaleFilters(), safe->GetChopFilters());
    }

    const char* SpectrumModelLabel(SpectrumParams::EnergySpectrumModel value)
    {
        const int index = static_cast<int>(value);
        if (index >= 0 && index < IM_ARRAYSIZE(kSpectrumModelLabels))
        {
            return kSpectrumModelLabels[index];
        }
        return "Unknown";
    }

    ImVec2 EqualizerGraphPoint(const ImVec2& min,
        const ImVec2& max,
        float x,
        float y,
        float yMax)
    {
        const float u = (x - EqualizerPreset::kXMin) / (EqualizerPreset::kXMax - EqualizerPreset::kXMin);
        const float v = std::clamp(y / std::max(0.001f, yMax), 0.0f, 1.0f);
        return ImVec2(
            min.x + std::clamp(u, 0.0f, 1.0f) * (max.x - min.x),
            max.y - v * (max.y - min.y));
    }

    void DrawEqualizerCurve(ImDrawList* drawList,
        const std::array<Math::float2, kEqualizerGraphSamples>& samples,
        int channel,
        const ImVec2& min,
        const ImVec2& max,
        float yMax,
        ImU32 fillColor,
        ImU32 lineColor)
    {
        std::array<ImVec2, kEqualizerGraphSamples> points{};
        for (int i = 0; i < kEqualizerGraphSamples; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(kEqualizerGraphSamples - 1);
            const float x = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u);
            const float y = channel == 0 ? samples[i].x : samples[i].y;
            points[i] = EqualizerGraphPoint(min, max, x, y, yMax);
        }

        const ImVec2 baseline0 = EqualizerGraphPoint(min, max, EqualizerPreset::kXMin, 1.0f, yMax);
        const ImVec2 baseline1 = EqualizerGraphPoint(min, max, EqualizerPreset::kXMax, 1.0f, yMax);
        for (int i = 1; i < kEqualizerGraphSamples; ++i)
        {
            const float t0 = static_cast<float>(i - 1) / static_cast<float>(kEqualizerGraphSamples - 1);
            const float t1 = static_cast<float>(i) / static_cast<float>(kEqualizerGraphSamples - 1);
            const ImVec2 base0(Math::Lerp(baseline0.x, baseline1.x, t0), baseline0.y);
            const ImVec2 base1(Math::Lerp(baseline0.x, baseline1.x, t1), baseline1.y);
            drawList->AddQuadFilled(base0, base1, points[i], points[i - 1], fillColor);
        }
        drawList->AddPolyline(points.data(), kEqualizerGraphSamples, lineColor, ImDrawFlags_None, 2.0f);
    }

    void DrawEqualizerVisualization(const char* id, const std::shared_ptr<EqualizerPreset>& preset)
    {
        if (!preset)
        {
            return;
        }

        ImGui::PushID(id);
        const float width = std::max(240.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 canvasSize(width, kEqualizerGraphHeight + kEqualizerGraphLabelHeight);
        const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##EqualizerGraph", canvasSize);
        const bool hovered = ImGui::IsItemHovered();

        const ImVec2 graphMin(canvasMin.x, canvasMin.y);
        const ImVec2 graphMax(canvasMin.x + canvasSize.x, canvasMin.y + kEqualizerGraphHeight);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        std::array<Math::float2, kEqualizerGraphSamples> samples{};
        float yMax = 2.0f;
        for (int i = 0; i < kEqualizerGraphSamples; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(kEqualizerGraphSamples - 1);
            samples[i] = preset->Sample(u);
            yMax = std::max(yMax, std::max(samples[i].x, samples[i].y));
        }
        yMax = std::ceil(yMax * 2.0f) * 0.5f;

        const ImU32 backgroundColor = IM_COL32(42, 44, 48, 255);
        const ImU32 gridColor = IM_COL32(106, 112, 122, 90);
        const ImU32 borderColor = IM_COL32(150, 156, 166, 180);
        const ImU32 baselineColor = IM_COL32(214, 218, 224, 170);
        const ImU32 labelColor = IM_COL32(196, 202, 212, 255);
        const ImU32 scaleFillColor = IM_COL32(54, 98, 160, 90);
        const ImU32 scaleLineColor = IM_COL32(129, 180, 254, 255);
        const ImU32 chopFillColor = IM_COL32(202, 87, 51, 85);
        const ImU32 chopLineColor = IM_COL32(252, 109, 64, 255);

        drawList->AddRectFilled(graphMin, graphMax, backgroundColor, 3.0f);

        for (float x = -1.5f; x <= 3.51f; x += 0.5f)
        {
            const ImVec2 top = EqualizerGraphPoint(graphMin, graphMax, x, yMax, yMax);
            const ImVec2 bottom = EqualizerGraphPoint(graphMin, graphMax, x, 0.0f, yMax);
            drawList->AddLine(top, bottom, gridColor);
        }
        for (float y = 0.0f; y <= yMax + 0.001f; y += 0.5f)
        {
            const ImVec2 left = EqualizerGraphPoint(graphMin, graphMax, EqualizerPreset::kXMin, y, yMax);
            const ImVec2 right = EqualizerGraphPoint(graphMin, graphMax, EqualizerPreset::kXMax, y, yMax);
            drawList->AddLine(left, right, y == 1.0f ? baselineColor : gridColor, y == 1.0f ? 1.5f : 1.0f);
        }

        DrawEqualizerCurve(drawList, samples, 0, graphMin, graphMax, yMax, scaleFillColor, scaleLineColor);
        DrawEqualizerCurve(drawList, samples, 1, graphMin, graphMax, yMax, chopFillColor, chopLineColor);

        drawList->AddRect(graphMin, graphMax, borderColor, 3.0f);

        constexpr float legendY = 8.0f;
        drawList->AddLine(ImVec2(graphMin.x + 10.0f, graphMin.y + legendY),
            ImVec2(graphMin.x + 28.0f, graphMin.y + legendY), scaleLineColor, 2.0f);
        drawList->AddText(ImVec2(graphMin.x + 34.0f, graphMin.y + legendY - 7.0f), labelColor, "Scale");
        drawList->AddLine(ImVec2(graphMin.x + 86.0f, graphMin.y + legendY),
            ImVec2(graphMin.x + 104.0f, graphMin.y + legendY), chopLineColor, 2.0f);
        drawList->AddText(ImVec2(graphMin.x + 110.0f, graphMin.y + legendY - 7.0f), labelColor, "Chop");

        const struct
        {
            float x;
            const char* label;
        } wavelengthTicks[] = {
            { -1.0f, "10 cm" },
            { 0.0f, "1 m" },
            { 1.0f, "10 m" },
            { 2.0f, "100 m" },
            { 3.0f, "1 km" },
        };
        for (const auto& tick : wavelengthTicks)
        {
            const ImVec2 p = EqualizerGraphPoint(graphMin, graphMax, tick.x, 0.0f, yMax);
            const ImVec2 textSize = ImGui::CalcTextSize(tick.label);
            drawList->AddText(ImVec2(p.x - textSize.x * 0.5f, graphMax.y + 3.0f), labelColor, tick.label);
        }

        char yLabel[32];
        std::snprintf(yLabel, sizeof(yLabel), "%.1fx", yMax);
        drawList->AddText(ImVec2(graphMax.x - ImGui::CalcTextSize(yLabel).x - 6.0f, graphMin.y + 4.0f), labelColor, yLabel);
        drawList->AddText(ImVec2(graphMax.x - 28.0f, EqualizerGraphPoint(graphMin, graphMax, 0.0f, 1.0f, yMax).y - 8.0f),
            labelColor,
            "1x");

        if (hovered)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (mouse.x >= graphMin.x && mouse.x <= graphMax.x && mouse.y >= graphMin.y && mouse.y <= graphMax.y)
            {
                const float u = std::clamp((mouse.x - graphMin.x) / std::max(1.0f, graphMax.x - graphMin.x), 0.0f, 1.0f);
                const float x = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u);
                const Math::float2 sample = preset->Sample(u);
                drawList->AddLine(ImVec2(mouse.x, graphMin.y), ImVec2(mouse.x, graphMax.y), IM_COL32(255, 255, 255, 110));

                ImGui::BeginTooltip();
                ImGui::Text("Wavelength: %s", FormatWavelength(std::pow(10.0f, x)).c_str());
                ImGui::Text("Scale: %.3fx", sample.x);
                ImGui::Text("Chop: %.3fx", sample.y);
                ImGui::Text("log10 wavelength: %.2f", x);
                ImGui::EndTooltip();
            }
        }

        ImGui::PopID();
    }

    float Float4Component(const Math::float4& value, int index)
    {
        const float* data = &value.x;
        return data[index];
    }

    float SpectrumPlotValue(const SpectrumParams& params, float depth, float x)
    {
        if (params.scale <= Math::EPS || params.windSpeed <= Math::EPS)
        {
            return 0.0f;
        }

        const float wavelength = std::pow(10.0f, x);
        const float k = Math::TWO_PI / std::max(wavelength, Math::EPS);
        const float omega = OceanSpectrum::Frequency(k, depth);
        if (omega <= Math::EPS)
        {
            return 0.0f;
        }

        float value = OceanSpectrum::FullSpectrum(omega, 0.0f, params, depth);
        value *= params.scale;
        value *= OceanSpectrum::ShortWavesFade(k, params.cutoffWavelength);
        return std::max(0.0f, value);
    }

    int CascadeForLogWavelength(const OceanSimulationSettings& settings, float x)
    {
        Math::float4 cutoffsLow;
        Math::float4 cutoffsHigh;
        settings.CalculateCascadeDomains(cutoffsLow, cutoffsHigh);

        const uint32_t cascadeCount = settings.GetCascadeCount();
        for (uint32_t i = 0; i < cascadeCount; ++i)
        {
            const float lowK = Float4Component(cutoffsLow, static_cast<int>(i));
            const float highK = Float4Component(cutoffsHigh, static_cast<int>(i));
            if (lowK <= Math::EPS || highK <= Math::EPS)
            {
                continue;
            }

            const float waveMin = std::log10(Math::TWO_PI / std::max(highK, Math::EPS));
            const float waveMax = std::log10(Math::TWO_PI / std::max(lowK, Math::EPS));
            if (x >= std::min(waveMin, waveMax) && x <= std::max(waveMin, waveMax))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    ImU32 CascadeColor(int cascade, int alpha)
    {
        switch (cascade)
        {
        case 0: return IM_COL32(241, 88, 84, alpha);
        case 1: return IM_COL32(250, 164, 58, alpha);
        case 2: return IM_COL32(96, 189, 104, alpha);
        case 3: return IM_COL32(93, 165, 218, alpha);
        default: return IM_COL32(14, 16, 18, alpha);
        }
    }

    void DrawSpectrumVisualization(const char* id,
        const OceanSimulationSettings& settings,
        const OceanSimulationInputs& inputs)
    {
        ImGui::PushID(id);
        const float width = std::max(300.0f, ImGui::GetContentRegionAvail().x);
        const ImVec2 canvasSize(width, kSpectrumPlotHeight + kSpectrumPlotLabelHeight);
        const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##SpectrumPlot", canvasSize);
        const bool hovered = ImGui::IsItemHovered();

        const ImVec2 graphMin(canvasMin.x, canvasMin.y);
        const ImVec2 graphMax(canvasMin.x + canvasSize.x, canvasMin.y + kSpectrumPlotHeight);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        std::array<float, kSpectrumPlotSamples> rawValues{};
        std::array<float, kSpectrumPlotSamples> normalizedValues{};
        float maxValue = 0.0f;
        const float depth = std::max(0.1f, inputs.depth);
        for (int i = 0; i < kSpectrumPlotSamples; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(kSpectrumPlotSamples - 1);
            const float x = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u);
            const float local = SpectrumPlotValue(inputs.local, depth, x);
            const float swell = SpectrumPlotValue(inputs.swell, depth, x);
            rawValues[i] = local + swell;
            maxValue = std::max(maxValue, rawValues[i]);
        }

        for (int i = 0; i < kSpectrumPlotSamples; ++i)
        {
            const float normalized = maxValue > Math::EPS ? rawValues[i] / maxValue : 0.0f;
            normalizedValues[i] = std::pow(std::clamp(normalized, 0.0f, 1.0f), 0.25f) * 0.95f;
        }

        const ImU32 backgroundColor = IM_COL32(42, 44, 48, 255);
        const ImU32 gridColor = IM_COL32(160, 166, 176, 90);
        const ImU32 borderColor = IM_COL32(180, 186, 196, 200);
        const ImU32 lineColor = IM_COL32(226, 234, 244, 235);
        const ImU32 labelColor = IM_COL32(200, 206, 216, 255);
        const ImU32 inactiveFillColor = IM_COL32(10, 12, 14, 205);

        drawList->AddRectFilled(graphMin, graphMax, backgroundColor, 3.0f);

        for (float x = -1.5f; x <= 3.51f; x += 0.5f)
        {
            const ImVec2 top = EqualizerGraphPoint(graphMin, graphMax, x, 1.0f, 1.0f);
            const ImVec2 bottom = EqualizerGraphPoint(graphMin, graphMax, x, 0.0f, 1.0f);
            drawList->AddLine(top, bottom, gridColor);
        }
        for (float y = 0.0f; y <= 1.001f; y += 0.25f)
        {
            const ImVec2 left = EqualizerGraphPoint(graphMin, graphMax, EqualizerPreset::kXMin, y, 1.0f);
            const ImVec2 right = EqualizerGraphPoint(graphMin, graphMax, EqualizerPreset::kXMax, y, 1.0f);
            drawList->AddLine(left, right, gridColor);
        }

        std::array<ImVec2, kSpectrumPlotSamples> points{};
        for (int i = 0; i < kSpectrumPlotSamples; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(kSpectrumPlotSamples - 1);
            const float x = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u);
            points[i] = EqualizerGraphPoint(graphMin, graphMax, x, normalizedValues[i], 1.0f);
        }

        for (int i = 1; i < kSpectrumPlotSamples; ++i)
        {
            const float u0 = static_cast<float>(i - 1) / static_cast<float>(kSpectrumPlotSamples - 1);
            const float u1 = static_cast<float>(i) / static_cast<float>(kSpectrumPlotSamples - 1);
            const float x0 = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u0);
            const float x1 = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u1);
            const float midX = (x0 + x1) * 0.5f;
            const int cascade = CascadeForLogWavelength(settings, midX);
            const ImU32 fillColor = cascade >= 0 ? CascadeColor(cascade, 155) : inactiveFillColor;
            const ImVec2 base0(points[i - 1].x, graphMax.y);
            const ImVec2 base1(points[i].x, graphMax.y);
            drawList->AddQuadFilled(base0, base1, points[i], points[i - 1], fillColor);
        }
        drawList->AddPolyline(points.data(), kSpectrumPlotSamples, lineColor, ImDrawFlags_None, 2.0f);
        drawList->AddRect(graphMin, graphMax, borderColor, 3.0f);

        const uint32_t cascadeCount = settings.GetCascadeCount();
        for (uint32_t i = 0; i < cascadeCount; ++i)
        {
            char label[32];
            std::snprintf(label, sizeof(label), "Cascade %u", i);
            drawList->AddText(ImVec2(graphMin.x + 12.0f, graphMin.y + 12.0f + static_cast<float>(i) * 17.0f),
                CascadeColor(static_cast<int>(i), 255),
                label);
        }

        const struct
        {
            float x;
            const char* label;
        } wavelengthTicks[] = {
            { -1.0f, "10 cm" },
            { 0.0f, "1 m" },
            { 1.0f, "10 m" },
            { 2.0f, "100 m" },
            { 3.0f, "1 km" },
        };
        for (const auto& tick : wavelengthTicks)
        {
            const ImVec2 p = EqualizerGraphPoint(graphMin, graphMax, tick.x, 0.0f, 1.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(tick.label);
            drawList->AddText(ImVec2(p.x - textSize.x * 0.5f, graphMax.y + 3.0f), labelColor, tick.label);
        }
        const char* axisLabel = "Wavelength";
        const ImVec2 axisSize = ImGui::CalcTextSize(axisLabel);
        drawList->AddText(ImVec2((graphMin.x + graphMax.x - axisSize.x) * 0.5f, graphMax.y + 17.0f), labelColor, axisLabel);

        if (hovered)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (mouse.x >= graphMin.x && mouse.x <= graphMax.x && mouse.y >= graphMin.y && mouse.y <= graphMax.y)
            {
                const float u = std::clamp((mouse.x - graphMin.x) / std::max(1.0f, graphMax.x - graphMin.x), 0.0f, 1.0f);
                const float x = Math::Lerp(EqualizerPreset::kXMin, EqualizerPreset::kXMax, u);
                const float local = SpectrumPlotValue(inputs.local, depth, x);
                const float swell = SpectrumPlotValue(inputs.swell, depth, x);
                const int cascade = CascadeForLogWavelength(settings, x);
                drawList->AddLine(ImVec2(mouse.x, graphMin.y), ImVec2(mouse.x, graphMax.y), IM_COL32(255, 255, 255, 120));

                ImGui::BeginTooltip();
                ImGui::Text("Wavelength: %s", FormatWavelength(std::pow(10.0f, x)).c_str());
                ImGui::Text("Local spectrum: %.4g", local);
                ImGui::Text("Swell spectrum: %.4g", swell);
                if (cascade >= 0)
                {
                    ImGui::Text("Cascade: %d", cascade);
                }
                else
                {
                    ImGui::TextUnformatted("Cascade: none");
                }
                ImGui::Text("log10 wavelength: %.2f", x);
                ImGui::EndTooltip();
            }
        }

        ImGui::PopID();
    }

    bool DrawSpectrumModelCombo(const char* label, SpectrumParams::EnergySpectrumModel& value)
    {
        bool changed = false;
        if (ImGui::BeginCombo(label, SpectrumModelLabel(value)))
        {
            for (int i = 0; i < IM_ARRAYSIZE(kSpectrumModelLabels); ++i)
            {
                const auto option = static_cast<SpectrumParams::EnergySpectrumModel>(i);
                const bool selected = option == value;
                if (ImGui::Selectable(kSpectrumModelLabels[i], selected))
                {
                    value = option;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool DrawFilterTypeCombo(const char* label, EqualizerPreset::FilterType& value)
    {
        bool changed = false;
        if (ImGui::BeginCombo(label, FilterTypeLabel(value)))
        {
            for (const FilterTypeOption& option : kFilterTypeOptions)
            {
                const bool selected = option.value == value;
                if (ImGui::Selectable(option.label, selected))
                {
                    value = option.value;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool DrawEqualizerFilterList(const char* label, std::vector<EqualizerPreset::Filter>& filters)
    {
        bool changed = false;
        if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (size_t i = 0; i < filters.size(); ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                EqualizerPreset::Filter& filter = filters[i];
                char nodeLabel[64];
                std::snprintf(nodeLabel, sizeof(nodeLabel), "%zu: %s###Filter%zu", i, FilterTypeLabel(filter.type), i);
                if (ImGui::TreeNodeEx(nodeLabel))
                {
                    changed |= DrawFilterTypeCombo("Type", filter.type);
                    changed |= ImGui::DragFloat("Center", &filter.center, 0.01f, EqualizerPreset::kXMin, EqualizerPreset::kXMax, "%.3f");
                    changed |= ImGui::DragFloat("Value", &filter.value, 0.01f, -4.0f, 4.0f, "%.3f");
                    changed |= ImGui::DragFloat("Width", &filter.width, 0.01f, EqualizerPreset::kMinWidth, 8.0f, "%.3f");
                    if (filter.width < EqualizerPreset::kMinWidth)
                    {
                        filter.width = EqualizerPreset::kMinWidth;
                    }
                    if (ImGui::Button("Remove"))
                    {
                        filters.erase(filters.begin() + static_cast<std::ptrdiff_t>(i));
                        changed = true;
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (ImGui::Button("Add filter"))
            {
                filters.push_back({});
                changed = true;
            }
            ImGui::TreePop();
        }
        return changed;
    }

    bool DrawEqualizerPreset(const char* id, std::shared_ptr<EqualizerPreset>& preset)
    {
        if (!preset)
        {
            preset = EqualizerPreset::CreateDefault();
        }

        std::vector<EqualizerPreset::Filter> scaleFilters = preset->GetScaleFilters();
        std::vector<EqualizerPreset::Filter> chopFilters = preset->GetChopFilters();

        bool changed = false;
        ImGui::PushID(id);
        changed |= DrawEqualizerFilterList("Scale filters", scaleFilters);
        changed |= DrawEqualizerFilterList("Chop filters", chopFilters);

        if (changed)
        {
            preset->SetScaleFilters(std::move(scaleFilters));
            preset->SetChopFilters(std::move(chopFilters));
        }
        DrawEqualizerVisualization("Preview", preset);
        ImGui::PopID();
        return changed;
    }

    bool DrawSpectrumParams(const char* id, SpectrumParams& params)
    {
        bool changed = false;
        ImGui::PushID(id);
        changed |= DrawSpectrumModelCombo("Energy model", params.energySpectrum);
        changed |= ImGui::DragFloat("Wind speed", &params.windSpeed, 0.05f, 0.0f, 40.0f, "%.2f");
        changed |= ImGui::DragFloat("Fetch", &params.fetch, 1.0f, 0.0f, 2000.0f, "%.1f");
        changed |= ImGui::DragFloat("Peaking", &params.peaking, 0.05f, 0.1f, 20.0f, "%.2f");
        changed |= ImGui::DragFloat("Scale", &params.scale, 0.01f, 0.0f, 10.0f, "%.3f");
        changed |= ImGui::DragFloat("Cutoff wavelength", &params.cutoffWavelength, 0.001f, 0.0f, 10.0f, "%.3f");
        changed |= ImGui::SliderFloat("Alignment", &params.alignment, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Extra alignment", &params.extraAlignment, 0.0f, 1.0f, "%.2f");
        ImGui::PopID();

        if (changed)
        {
            params.windSpeed = std::max(0.0f, params.windSpeed);
            params.fetch = std::max(0.0f, params.fetch);
            params.peaking = std::max(0.1f, params.peaking);
            params.scale = std::max(0.0f, params.scale);
            params.cutoffWavelength = std::max(0.0f, params.cutoffWavelength);
            params.alignment = Math::Saturate(params.alignment);
            params.extraAlignment = Math::Saturate(params.extraAlignment);
        }

        return changed;
    }

    bool DrawFoamParams(FoamParams& foam)
    {
        bool changed = false;
        changed |= ImGui::DragFloat("Decay rate", &foam.decayRate, 0.001f, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat("Coverage", &foam.coverage, 0.005f, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat("Density", &foam.density, 0.05f, 0.0f, 64.0f, "%.2f");
        changed |= ImGui::SliderFloat("Sharpness", &foam.sharpness, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::SliderFloat("Persistence", &foam.persistence, 0.0f, 1.0f, "%.2f");
        changed |= ImGui::DragFloat("Trail", &foam.trail, 0.005f, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat("Trail texture strength", &foam.trailTextureStrength, 0.005f, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat2("Trail texture size", &foam.trailTextureSize.x, 1.0f, 1.0f, 1000.0f, "%.1f");
        changed |= ImGui::DragFloat("Underwater", &foam.underwater, 0.005f, 0.0f, 2.0f, "%.3f");
        changed |= ImGui::DragFloat4("Cascade weights", &foam.cascadesWeights.x, 0.01f, 0.0f, 8.0f, "%.2f");

        if (changed)
        {
            foam.decayRate = std::max(0.0f, foam.decayRate);
            foam.coverage = std::max(0.0f, foam.coverage);
            foam.density = std::max(0.0f, foam.density);
            foam.sharpness = Math::Saturate(foam.sharpness);
            foam.persistence = Math::Saturate(foam.persistence);
            foam.trail = std::max(0.0f, foam.trail);
            foam.trailTextureStrength = std::max(0.0f, foam.trailTextureStrength);
            foam.trailTextureSize.x = std::max(1.0f, foam.trailTextureSize.x);
            foam.trailTextureSize.y = std::max(1.0f, foam.trailTextureSize.y);
            foam.underwater = std::max(0.0f, foam.underwater);
            foam.cascadesWeights.x = std::max(0.0f, foam.cascadesWeights.x);
            foam.cascadesWeights.y = std::max(0.0f, foam.cascadesWeights.y);
            foam.cascadesWeights.z = std::max(0.0f, foam.cascadesWeights.z);
            foam.cascadesWeights.w = std::max(0.0f, foam.cascadesWeights.w);
        }

        return changed;
    }

    bool DrawOceanSettingsControls(Renderer& renderer, OceanSimulation& ocean)
    {
        OceanSimulationSettings edited = ocean.GetSettings();
        bool settingsChanged = false;

        auto resolutionValue = static_cast<OceanSimulationSettings::ResolutionValue>(edited.GetResolution());
        if (ImGui::BeginCombo("Resolution", ResolutionLabel(resolutionValue)))
        {
            for (const ResolutionOption& option : kResolutionOptions)
            {
                const bool selected = option.value == resolutionValue;
                if (ImGui::Selectable(option.label, selected))
                {
                    edited.SetResolution(option.value);
                    resolutionValue = option.value;
                    settingsChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        auto cascadeValue = static_cast<OceanSimulationSettings::CascadesNumberValue>(edited.GetCascadeCount());
        if (ImGui::BeginCombo("Cascades", CascadeCountLabel(cascadeValue)))
        {
            for (const CascadeCountOption& option : kCascadeCountOptions)
            {
                const bool selected = option.value == cascadeValue;
                if (ImGui::Selectable(option.label, selected))
                {
                    edited.SetCascadeCount(option.value);
                    cascadeValue = option.value;
                    settingsChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        int anisotropy = static_cast<int>(edited.GetAnisotropyLevel());
        if (ImGui::DragInt("Anisotropy", &anisotropy, 1.0f, 1, 16))
        {
            edited.SetAnisotropyLevel(static_cast<uint32_t>(std::max(1, anisotropy)));
            settingsChanged = true;
        }

        bool simulateFoam = edited.ShouldSimulateFoam();
        if (ImGui::Checkbox("Simulate foam", &simulateFoam))
        {
            edited.SetSimulateFoam(simulateFoam);
            settingsChanged = true;
        }

        bool updateSpectrum = edited.ShouldUpdateSpectrum();
        if (ImGui::Checkbox("Update spectrum each frame", &updateSpectrum))
        {
            edited.SetUpdateSpectrum(updateSpectrum);
            settingsChanged = true;
        }

        OceanSimulationSettings::ReadbackCascadesMode readbackMode = edited.GetReadbackMode();
        if (ImGui::BeginCombo("Readback cascades", ReadbackModeLabel(readbackMode)))
        {
            for (const ReadbackModeOption& option : kReadbackModeOptions)
            {
                const bool selected = option.value == readbackMode;
                if (ImGui::Selectable(option.label, selected))
                {
                    edited.SetReadbackMode(option.value);
                    readbackMode = option.value;
                    settingsChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        int samplingIterations = static_cast<int>(edited.GetSamplingIterations());
        if (ImGui::DragInt("Sampling iterations", &samplingIterations, 1.0f, 1, 64))
        {
            edited.SetSamplingIterations(static_cast<uint32_t>(std::max(1, samplingIterations)));
            settingsChanged = true;
        }

        OceanSimulationSettings::CascadeDomainsMode domainMode = edited.GetDomainsMode();
        if (ImGui::BeginCombo("Domain mode", DomainModeLabel(domainMode)))
        {
            for (const DomainModeOption& option : kDomainModeOptions)
            {
                const bool selected = option.value == domainMode;
                if (ImGui::Selectable(option.label, selected))
                {
                    edited.SetDomainsMode(option.value);
                    domainMode = option.value;
                    settingsChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        float simulationScale = edited.GetSimulationScale();
        ImGui::BeginDisabled(domainMode != OceanSimulationSettings::CascadeDomainsMode::Auto);
        if (ImGui::DragFloat("Simulation scale", &simulationScale, 1.0f, 1.0f, 10000.0f, "%.1f"))
        {
            edited.SetSimulationScale(std::max(1.0f, simulationScale));
            settingsChanged = true;
        }
        ImGui::EndDisabled();

        Math::float4 manualScales = edited.GetManualLengthScales();
        ImGui::BeginDisabled(domainMode != OceanSimulationSettings::CascadeDomainsMode::Manual);
        if (ImGui::DragFloat4("Manual length scales", &manualScales.x, 0.1f, 1.0f, 10000.0f, "%.2f"))
        {
            manualScales.x = std::max(1.0f, manualScales.x);
            manualScales.y = std::max(1.0f, manualScales.y);
            manualScales.z = std::max(1.0f, manualScales.z);
            manualScales.w = std::max(1.0f, manualScales.w);
            edited.SetManualLengthScales(manualScales);
            settingsChanged = true;
        }

        bool allowOverlap = edited.AllowOverlap();
        if (ImGui::Checkbox("Allow cascade overlap", &allowOverlap))
        {
            edited.SetAllowOverlap(allowOverlap);
            settingsChanged = true;
        }

        float minWaves = edited.GetMinWavesInCascade();
        if (ImGui::DragFloat("Min waves in cascade", &minWaves, 0.05f, 1.0f, 32.0f, "%.2f"))
        {
            edited.SetMinWavesInCascade(std::max(1.0f, minWaves));
            settingsChanged = true;
        }
        ImGui::EndDisabled();

        Math::float4 lengthScales = edited.ComputeLengthScales();
        Math::float4 cutoffsLow;
        Math::float4 cutoffsHigh;
        edited.CalculateCascadeDomains(cutoffsLow, cutoffsHigh);
        ImGui::Text("Length scales: %.2f  %.2f  %.2f  %.2f",
            lengthScales.x, lengthScales.y, lengthScales.z, lengthScales.w);
        ImGui::Text("Cutoff low: %.3f  %.3f  %.3f  %.3f",
            cutoffsLow.x, cutoffsLow.y, cutoffsLow.z, cutoffsLow.w);
        ImGui::Text("Cutoff high: %.3f  %.3f  %.3f  %.3f",
            cutoffsHigh.x, cutoffsHigh.y, cutoffsHigh.z, cutoffsHigh.w);

        if (ImGui::TreeNodeEx("Spectrum plot", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawSpectrumVisualization("SimulationSpectrum", edited, ocean.EvaluateInputs());
            ImGui::TreePop();
        }

        if (settingsChanged)
        {
            ocean.SetSettings(&renderer, edited);
        }
        return settingsChanged;
    }

    bool DrawOceanSceneControls(Renderer& renderer, OceanSimulation& ocean)
    {
        float localWind = ocean.GetLocalWindDirectionDegrees();
        float swellDirection = ocean.GetSwellDirectionDegrees();
        float windForce = ocean.GetWindForce01();

        bool changed = false;
        changed |= ImGui::DragFloat("Local wind direction", &localWind, 0.25f, -360.0f, 360.0f, "%.1f deg");
        changed |= ImGui::DragFloat("Swell direction", &swellDirection, 0.25f, -360.0f, 360.0f, "%.1f deg");
        changed |= ImGui::SliderFloat("Wind force", &windForce, 0.0f, 1.0f, "%.2f");

        if (changed)
        {
            ocean.SetSceneVariables(&renderer, localWind, swellDirection, windForce);
        }
        return changed;
    }

    bool DrawOceanRenderControls(OceanSimulation& ocean)
    {
        OceanRenderConfig render = ocean.GetRenderConfig();
        bool changed = false;

        const auto drawColor = [&changed](const char* label, Math::float4& color)
        {
            float values[3] = { color.x, color.y, color.z };
            if (!ImGui::ColorEdit3(label, values, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
            {
                return;
            }

            color = Math::float4(values[0], values[1], values[2], color.w);
            changed = true;
        };
        const auto drag = [&changed](const char* label,
            float& value,
            float speed,
            float minimum,
            float maximum,
            const char* format = "%.3f")
        {
            if (ImGui::DragFloat(label, &value, speed, minimum, maximum, format))
            {
                value = std::clamp(value, minimum, maximum);
                changed = true;
            }
        };
        const auto dragVector4 = [&changed](const char* label,
            Math::float4& value,
            float speed,
            float minimum,
            float maximum)
        {
            float values[4] = { value.x, value.y, value.z, value.w };
            if (ImGui::DragFloat4(label, values, speed, minimum, maximum, "%.3f"))
            {
                value = Math::float4(
                    std::clamp(values[0], minimum, maximum),
                    std::clamp(values[1], minimum, maximum),
                    std::clamp(values[2], minimum, maximum),
                    std::clamp(values[3], minimum, maximum));
                changed = true;
            }
        };

        ImGui::SeparatorText("Surface color");
        drawColor("Deep scatter tint", render.deepScatterColor);
        drawColor("Subsurface tint", render.sssColor);
        drawColor("Diffuse tint", render.diffuseColor);

        ImGui::SeparatorText("Specular and reflection");
        drag("Specular strength", render.specularStrength, 0.01f, 0.0f, 10.0f);
        drag("Roughness scale", render.roughnessScale, 0.005f, 0.0f, 1.0f);
        drag("Roughness distance", render.roughnessDistance, 1.0f, 1.0f, 5000.0f, "%.1f");
        drag("Reflection normal flattening", render.reflectionNormalStrength, 0.005f, 0.0f, 1.0f);
        drag("Horizon fog strength", render.horizonFogStrength, 0.005f, 0.01f, 5.0f);
        drag("Horizon fog distance scale", render.horizonFogDistanceScale, 0.01f, 0.0f, 20.0f);
        drag("Cascade fade scale", render.cascadeFadeScale, 0.1f, 0.0f, 1000.0f);
        drag("Minimum mesh scale", render.minMeshScale, 0.1f, 0.001f, 1000.0f);
        drag("Detail normal mip bias", render.detailNormalMipBias, 0.05f, -4.0f, 4.0f);
        drag("Macro normal mip bias (DLSS)", render.macroNormalMipBiasDlss, 0.05f, -4.0f, 4.0f);
        drag("Macro normal mip bias (native)", render.macroNormalMipBiasNative, 0.05f, -4.0f, 4.0f);

        ImGui::SeparatorText("Refraction and volume");
        drag("Surface refraction", render.surfaceRefractionStrength, 0.005f, 0.0f, 5.0f);
        drag("Underwater refraction", render.underwaterRefractionStrength, 0.005f, 0.0f, 5.0f);
        drag("Absorption depth", render.absorptionDepthScale, 0.1f, 1.0f, 1000.0f);
        drag("Fog density", render.fogDensity, 0.005f, 0.0f, 10.0f);

        ImGui::SeparatorText("Subsurface scattering");
        drag("Sun scatter", render.sunScatterStrength, 0.01f, 0.0f, 10.0f);
        drag("Sky scatter", render.skyScatterStrength, 0.01f, 0.0f, 10.0f);
        drag("Scatter spread", render.scatterSpread, 0.005f, 0.001f, 2.0f);
        drag("View alignment", render.viewAlignmentStrength, 0.005f, 0.0f, 1.0f);
        drag("SSS height bias", render.sssHeightBias, 0.01f, -10.0f, 10.0f);
        drag("SSS distance fade", render.sssFadeDistance, 0.1f, 0.0f, 1000.0f);

        ImGui::SeparatorText("Wind shading");
        drag("Wind speed", render.windSpeed, 0.1f, 0.0f, 100.0f);
        drag("Waves scale", render.wavesScale, 0.01f, 0.0f, 10.0f);
        drag("Wind alignment", render.windAlignment, 0.005f, 0.0f, 1.0f);
        drag("UV warp strength", render.windUvWarpStrength, 0.005f, 0.0f, 5.0f);

        ImGui::SeparatorText("Shore and surf");
        // The two surface variants read DIFFERENT settings, so the section only shows the live
        // ones. The legacy (June-22) surface has its damping built in and exactly one authored
        // shore knob; everything below the else is modern-only and would be silently inert there.
        if (!ocean::g_shoreRunup)
        {
            ImGui::TextDisabled(
                "Legacy ocean surface (relaunch with --ocean-runup-shore for the modern stack).");
            drag(
                "Contact foam strength",
                render.shoreLegacyContactFoamStrength,
                0.002f,
                0.0f,
                1.0f);
        }
        else
        {
        drag("Vertical fade depth", render.shoreVerticalFadeDepth, 0.01f, 0.01f, 10.0f);
        drag("Shallow XZ strength", render.shoreHorizontalMin, 0.005f, 0.0f, 1.0f);
        drag("XZ restore depth", render.shoreHorizontalFadeDepth, 0.01f, 0.01f, 10.0f);
        drag("Normal fade depth", render.shoreNormalFadeDepth, 0.01f, 0.01f, 10.0f);
        dragVector4("Normal cascade minimums", render.shoreNormalMinWeights, 0.005f, 0.0f, 1.0f);
        drag("Run-up depth", render.shoreRunupDepth, 0.01f, 0.01f, 10.0f);
        drag("Run-up strength", render.shoreRunupStrength, 0.01f, 0.0f, 10.0f);
        drag("Run-up max wave", render.shoreRunupMaxWave, 0.01f, 0.0f, 10.0f);
        drag("Run-up slope fade start", render.shoreRunupSlopeStartDegrees, 0.25f, 0.0f, 89.0f);
        drag("Run-up slope cutoff", render.shoreRunupSlopeEndDegrees, 0.25f, 0.0f, 89.0f);
        drag("Swash amplitude", render.shoreSwashAmplitude, 0.005f, 0.0f, 1.0f);
        drag("Run-up slope smoothing", render.shoreRunupSlopeSmoothing, 0.05f, 0.5f, 8.0f);
        drag("Bottom clearance", render.shoreBottomClearance, 0.005f, 0.0f, 1.0f);
        drag("Refraction soft edge distance", render.shoreEdgeSoftDepth, 0.001f, 0.0f, 0.25f);
        drag(
            "Geometry edge refraction fade",
            render.shoreGeometryEdgeRefractionFadeDepth,
            0.005f,
            0.0f,
            2.0f);
        drag(
            "Geometry wave fade distance",
            render.shoreGeometryFadeDistance,
            1.0f,
            1.0f,
            2000.0f,
            "%.1f");

        if (ImGui::TreeNodeEx(
            "Contact foam",
            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth))
        {
            ImGui::SeparatorText("Coverage");
            drag("Main width", render.shoreContactFoamMainWidth, 0.002f, 0.0f, 1.0f);
            drag(
                "Main breakup length",
                render.shoreContactFoamBreakupLength,
                0.001f,
                0.0f,
                1.0f);
            drag(
                "Breakup length variation",
                render.shoreContactFoamBreakupLengthVariation,
                0.002f,
                0.0f,
                1.0f);
            drag(
                "Breakup variation scale",
                render.shoreContactFoamBreakupVariationScale,
                0.002f,
                0.001f,
                1.0f);
            drag("Opacity", render.shoreContactFoamOpacity, 0.005f, 0.0f, 1.0f);

            ImGui::SeparatorText("Wind response");
            // The serialized field is still `shoreContactFoamCalmAmount` so existing ocean configs
            // keep loading; only its MEANING changed (a wind threshold, not a calm-time multiplier).
            drag(
                "No foam below wind force",
                render.shoreContactFoamCalmAmount,
                0.005f,
                0.0f,
                1.0f);
            drag(
                "Full at wind force",
                render.shoreContactFoamFullWindForce,
                0.005f,
                0.01f,
                1.0f);

            ImGui::SeparatorText("Breakup pattern");
            drag("Pattern scale", render.shoreContactFoamPatternScale, 0.005f, 0.001f, 2.0f);
            drag("Pattern density", render.shoreContactFoamPatternDensity, 0.005f, 0.0f, 1.0f);
            drag("Pattern scroll speed", render.shoreContactFoamPatternScrollSpeed, 0.01f, 0.0f, 10.0f);

            ImGui::SeparatorText("Signed depth warp");
            drag(
                "Depth warp scale",
                render.shoreContactFoamDepthWarpScale,
                0.002f,
                0.001f,
                2.0f);
            drag(
                "Depth warp strength",
                render.shoreContactFoamDepthWarpStrength,
                0.002f,
                0.0f,
                0.5f);
            drag(
                "Depth warp range",
                render.shoreContactFoamDepthWarpRange,
                0.005f,
                0.0f,
                2.0f);

            ImGui::SeparatorText("Appearance");
            drag("Albedo scale", render.shoreContactFoamAlbedoScale, 0.01f, 0.001f, 10.0f);
            drag("Albedo scroll speed", render.shoreContactFoamAlbedoScrollSpeed, 0.01f, 0.0f, 10.0f);
            drag(
                "Normal strength",
                render.shoreContactFoamNormalStrength,
                0.005f,
                0.0f,
                1.0f);

            ImGui::SeparatorText("Diagnostics");
            // Deliberately outside `render` — this is a debugging knob, not a setting, so it must
            // never end up serialized into the ocean config.
            static const char* kFoamDebugViews[] = {
                "Off (normal shading)",
                "1 Sweep t (solid=green, tail=heat, past end=magenta)",
                "2 Feather used (red = clamped at max)",
                "3 RAW fwidth(vertex depth) - the unclamped source",
                "4 Tear noise",
                "5 Contact coverage",
                "6 Weights: R=field G=fallback B=depth",
                "7 Shore depth, 10 cm contours (blue = above water)",
                "8 Tail length (0..2 m)",
                "9 Sweep t + shore-depth texel grid",
                "10 Coverage, OLD pixel-shader depth lookup (facets)",
                "11 (unused)",
                "12 R=out-of-field regime, G=field weight",
                "13 Shore SDF (green = water, red = inland, 2 m bands)",
                "14 Contact depth, 1 mm contours (sawtooth = quantized field)",
            };
            ImGui::Combo(
                "Foam debug view",
                &ocean::g_foamDebugView,
                kFoamDebugViews,
                static_cast<int>(std::size(kFoamDebugViews)));
            if (!ocean::g_foamDebug)
            {
                ImGui::TextDisabled("inert: relaunch with --ocean-foam-debug");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "The views are a shader VARIANT so the shipping shader carries none of\n"
                        "their cost. Switching views once the variant is compiled is free.");
                }
            }
            ImGui::TreePop();
        }
        } // modern shore stack (ocean::g_shoreRunup)

        ImGui::SeparatorText("Foam");
        drawColor("Foam tint", render.foamTint);
        drag("Foam normal strength", render.foamNormalStrength, 0.005f, 0.0f, 1.0f);
        drag("Underwater foam parallax", render.underwaterFoamParallax, 0.01f, 0.0f, 10.0f);

        ImGui::SeparatorText("Caustics");
        if (ImGui::Checkbox("Caustics enabled", &render.causticsEnabled))
        {
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Sun caustics on everything below the water line.\n"
                              "Applied in the deferred lighting pass, so they follow the sun shadow.");
        }
        drag("Intensity", render.causticsIntensity, 0.01f, 0.0f, 6.0f);
        drag("Tile size (m)", render.causticsScale, 0.05f, 0.25f, 60.0f);
        drag("Speed (frames/s)", render.causticsSpeed, 0.1f, 0.0f, 60.0f);
        drag("Depth fade (m)", render.causticsDepthFade, 0.1f, 0.1f, 120.0f);
        drag("Surface fade (m)", render.causticsSurfaceFade, 0.01f, 0.0f, 5.0f);
        drag("Up-facing gate", render.causticsUpFacing, 0.005f, 0.0f, 1.0f);
        drag("Dark bias", render.causticsBias, 0.005f, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Pattern value that means \"no gain\". Above it the filaments brighten,\n"
                              "below it the cells between them darken.");
        }
        drag("Dispersion (texels)", render.causticsDispersion, 0.01f, 0.0f, 8.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Chromatic split of the filament edges. 0 is monochrome and 3x cheaper\n"
                              "(the pattern is sampled once per channel).");
        }
        drag("De-tile layer", render.causticsLayerBlend, 0.005f, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Blends in a second layer at another scale, min-combined, to hide the\n"
                              "tiling period. 0 halves the texture taps.");
        }
        drawColor("Caustics tint", render.causticsTint);

        ImGui::SeparatorText("Absorption gradient");
        bool curvedGradient = render.absorptionGradientType >= 0.5f;
        if (ImGui::Checkbox("Curved interpolation", &curvedGradient))
        {
            render.absorptionGradientType = curvedGradient ? 1.0f : 0.0f;
            changed = true;
        }
        for (size_t index = 0; index < render.absorptionColors.size(); ++index)
        {
            ImGui::PushID(static_cast<int>(index));
            Math::float4& key = render.absorptionColors[index];
            drawColor("Color", key);
            drag("Position", key.w, 0.005f, 0.0f, 1.0f);
            ImGui::Separator();
            ImGui::PopID();
        }
        if (render.absorptionColors.size() < 8u && ImGui::Button("Add absorption key"))
        {
            const Math::float4 last = render.absorptionColors.empty()
                ? Math::float4(1.0f, 1.0f, 1.0f, 1.0f)
                : render.absorptionColors.back();
            render.absorptionColors.push_back(last);
            changed = true;
        }
        if (render.absorptionColors.size() > 1u)
        {
            ImGui::SameLine();
            if (ImGui::Button("Remove absorption key"))
            {
                render.absorptionColors.pop_back();
                changed = true;
            }
        }

        if (changed)
        {
            std::stable_sort(
                render.absorptionColors.begin(),
                render.absorptionColors.end(),
                [](const Math::float4& lhs, const Math::float4& rhs)
                {
                    return lhs.w < rhs.w;
                });
            ocean.SetRenderConfig(render);
        }
        return changed;
    }

    void SanitizeConfigSelection(OceanSimulationConfig& config)
    {
        if (!config.defaultEqualizer)
        {
            config.defaultEqualizer = EqualizerPreset::CreateDefault();
        }
        if (!config.swellPreset)
        {
            config.swellPreset = std::make_shared<SwellPreset>();
        }
        if (config.localPresets.empty())
        {
            config.localPresets.push_back(config.localPreset ? config.localPreset : std::make_shared<LocalWavesPreset>());
        }

        config.localPresetIndex = std::min(config.localPresetIndex, config.localPresets.size() - 1u);
        config.localPreset = config.localPresets[config.localPresetIndex];
        if (!config.localPreset->GetEqualizer())
        {
            config.localPreset->SetEqualizer(config.defaultEqualizer);
        }
    }

    std::string LocalPresetLabel(size_t index, const std::shared_ptr<LocalWavesPreset>& preset)
    {
        char label[128];
        if (preset)
        {
            std::snprintf(label, sizeof(label), "%zu: wind %.2f  height %.2f  chop %.2f",
                index,
                preset->GetWindForce(),
                preset->GetReferenceWaveHeight(),
                preset->GetChop());
        }
        else
        {
            std::snprintf(label, sizeof(label), "%zu: empty", index);
        }
        return label;
    }

    bool DrawLocalPresetControls(OceanSimulationConfig& config,
        std::vector<std::shared_ptr<EqualizerPreset>>& localEqualizerBackups)
    {
        SanitizeConfigSelection(config);
        localEqualizerBackups.resize(config.localPresets.size());

        bool changed = false;
        const std::string currentLabel = LocalPresetLabel(config.localPresetIndex, config.localPreset);
        if (ImGui::BeginCombo("Selected local preset", currentLabel.c_str()))
        {
            for (size_t i = 0; i < config.localPresets.size(); ++i)
            {
                const std::string label = LocalPresetLabel(i, config.localPresets[i]);
                const bool selected = i == config.localPresetIndex;
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    config.localPresetIndex = i;
                    config.localPreset = config.localPresets[i];
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Add preset"))
        {
            auto preset = std::make_shared<LocalWavesPreset>();
            preset->SetEqualizer(config.defaultEqualizer);
            if (!config.localPresets.empty() && config.localPresets.back())
            {
                preset->SetWindForce(config.localPresets.back()->GetWindForce() + 1.0f);
            }
            config.localPresets.push_back(preset);
            localEqualizerBackups.push_back(nullptr);
            config.localPresetIndex = config.localPresets.size() - 1u;
            config.localPreset = preset;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Duplicate"))
        {
            std::shared_ptr<EqualizerPreset> backup;
            if (config.localPresetIndex < localEqualizerBackups.size() && localEqualizerBackups[config.localPresetIndex])
            {
                backup = CloneEqualizerPreset(localEqualizerBackups[config.localPresetIndex]);
            }
            else if (config.localPreset && config.localPreset->GetEqualizer() &&
                config.localPreset->GetEqualizer() != config.defaultEqualizer)
            {
                backup = CloneEqualizerPreset(config.localPreset->GetEqualizer());
            }
            auto preset = std::make_shared<LocalWavesPreset>(*config.localPreset);
            config.localPresets.insert(config.localPresets.begin() + static_cast<std::ptrdiff_t>(config.localPresetIndex + 1u), preset);
            localEqualizerBackups.insert(localEqualizerBackups.begin() + static_cast<std::ptrdiff_t>(config.localPresetIndex + 1u), backup);
            config.localPresetIndex += 1u;
            config.localPreset = preset;
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(config.localPresets.size() <= 1u);
        if (ImGui::Button("Remove"))
        {
            config.localPresets.erase(config.localPresets.begin() + static_cast<std::ptrdiff_t>(config.localPresetIndex));
            localEqualizerBackups.erase(localEqualizerBackups.begin() + static_cast<std::ptrdiff_t>(config.localPresetIndex));
            config.localPresetIndex = std::min(config.localPresetIndex, config.localPresets.size() - 1u);
            config.localPreset = config.localPresets[config.localPresetIndex];
            changed = true;
        }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(config.localPresetIndex == 0u);
        if (ImGui::Button("Previous"))
        {
            config.localPresetIndex -= 1u;
            config.localPreset = config.localPresets[config.localPresetIndex];
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(config.localPresetIndex + 1u >= config.localPresets.size());
        if (ImGui::Button("Next"))
        {
            config.localPresetIndex += 1u;
            config.localPreset = config.localPresets[config.localPresetIndex];
            changed = true;
        }
        ImGui::EndDisabled();

        LocalWavesPreset& preset = *config.localPreset;
        float windForce = preset.GetWindForce();
        if (ImGui::DragFloat("Preset wind force", &windForce, 0.01f, 0.0f, 100.0f, "%.2f"))
        {
            preset.SetWindForce(std::max(0.0f, windForce));
            changed = true;
        }

        float referenceHeight = preset.GetReferenceWaveHeight();
        if (ImGui::DragFloat("Reference wave height", &referenceHeight, 0.01f, 0.0f, 20.0f, "%.2f"))
        {
            preset.SetReferenceWaveHeight(std::max(0.0f, referenceHeight));
            changed = true;
        }

        float chop = preset.GetChop();
        if (ImGui::DragFloat("Chop", &chop, 0.01f, 0.0f, 5.0f, "%.2f"))
        {
            preset.SetChop(std::max(0.0f, chop));
            changed = true;
        }

        if (ImGui::TreeNodeEx("Local spectrum", ImGuiTreeNodeFlags_DefaultOpen))
        {
            SpectrumParams spectrum = preset.GetSpectrum();
            if (DrawSpectrumParams("LocalSpectrum", spectrum))
            {
                preset.SetSpectrum(spectrum);
                changed = true;
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Local foam", ImGuiTreeNodeFlags_DefaultOpen))
        {
            FoamParams foam = preset.GetFoam();
            if (DrawFoamParams(foam))
            {
                preset.SetFoam(foam);
                changed = true;
            }
            ImGui::TreePop();
        }

        std::shared_ptr<EqualizerPreset>& customEqualizerBackup = localEqualizerBackups[config.localPresetIndex];
        const bool hadActiveCustomEqualizer = preset.GetEqualizer() && preset.GetEqualizer() != config.defaultEqualizer;
        bool useDefaultEqualizer = !hadActiveCustomEqualizer;
        if (ImGui::Checkbox("Use default equalizer", &useDefaultEqualizer))
        {
            if (useDefaultEqualizer)
            {
                if (hadActiveCustomEqualizer)
                {
                    customEqualizerBackup = CloneEqualizerPreset(preset.GetEqualizer());
                }
                preset.SetEqualizer(config.defaultEqualizer);
            }
            else
            {
                if (!customEqualizerBackup)
                {
                    customEqualizerBackup = CloneEqualizerPreset(config.defaultEqualizer);
                }
                preset.SetEqualizer(CloneEqualizerPreset(customEqualizerBackup));
            }
            changed = true;
        }

        if (!useDefaultEqualizer && ImGui::TreeNodeEx("Local equalizer"))
        {
            std::shared_ptr<EqualizerPreset> equalizer = preset.GetEqualizer();
            if (DrawEqualizerPreset("LocalEqualizer", equalizer))
            {
                preset.SetEqualizer(equalizer);
                customEqualizerBackup = CloneEqualizerPreset(equalizer);
                changed = true;
            }
            ImGui::TreePop();
        }

        return changed;
    }

    bool DrawOceanInputControls(Renderer& renderer,
        OceanSimulation& ocean,
        std::vector<std::shared_ptr<EqualizerPreset>>& localEqualizerBackups)
    {
        OceanSimulationConfig config = ocean.GetConfigCopy();
        SanitizeConfigSelection(config);
        bool configChanged = false;

        OceanSimulationInputsProvider::InputsProviderMode mode = config.inputMode;
        if (ImGui::BeginCombo("Input mode", InputModeLabel(mode)))
        {
            for (const InputModeOption& option : kInputModeOptions)
            {
                const bool selected = option.value == mode;
                if (ImGui::Selectable(option.label, selected))
                {
                    config.inputMode = option.value;
                    mode = option.value;
                    configChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        float timeScale = config.timeScale;
        if (ImGui::DragFloat("Time scale", &timeScale, 0.01f, 0.0f, 20.0f, "%.3f"))
        {
            config.timeScale = std::max(0.0f, timeScale);
            configChanged = true;
        }

        float depth = config.depth;
        if (ImGui::DragFloat("Water depth", &depth, 1.0f, 0.1f, 10000.0f, "%.1f"))
        {
            config.depth = std::max(0.1f, depth);
            configChanged = true;
        }

        if (ImGui::TreeNodeEx("Default equalizer"))
        {
            configChanged |= DrawEqualizerPreset("DefaultEqualizer", config.defaultEqualizer);
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Swell", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool swellChanged = false;
            SpectrumParams spectrum = config.swellPreset->GetSpectrum();
            swellChanged |= DrawSpectrumParams("SwellSpectrum", spectrum);
            float referenceHeight = config.swellPreset->GetReferenceWaveHeight();
            swellChanged |= ImGui::DragFloat("Reference wave height", &referenceHeight, 0.01f, 0.0f, 20.0f, "%.2f");
            if (swellChanged)
            {
                config.swellPreset->SetSpectrum(spectrum);
                config.swellPreset->SetReferenceWaveHeight(std::max(0.0f, referenceHeight));
                configChanged = true;
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Local presets", ImGuiTreeNodeFlags_DefaultOpen))
        {
            configChanged |= DrawLocalPresetControls(config, localEqualizerBackups);
            ImGui::TreePop();
        }

        if (configChanged)
        {
            SanitizeConfigSelection(config);
            ocean.ApplyConfig(&renderer, config);
        }

        const OceanSimulationInputs evaluated = ocean.EvaluateInputs();
        ImGui::Separator();
        ImGui::Text("Evaluated: height %.2f  chop %.2f  depth %.1f  time %.3f",
            evaluated.referenceWaveHeight,
            evaluated.chop,
            evaluated.depth,
            evaluated.timeScale);
        ImGui::Text("Local: wind %.2f  scale %.3f  cutoff %.3f",
            evaluated.local.windSpeed,
            evaluated.local.scale,
            evaluated.local.cutoffWavelength);
        ImGui::Text("Swell: wind %.2f  scale %.3f  cutoff %.3f",
            evaluated.swell.windSpeed,
            evaluated.swell.scale,
            evaluated.swell.cutoffWavelength);
        return configChanged;
    }

    bool DrawOceanControlsContent(Renderer& renderer,
        OceanSimulation& ocean,
        std::vector<std::shared_ptr<EqualizerPreset>>& localEqualizerBackups)
    {
        bool configChanged = false;
        if (ImGui::Button("Reset initial spectrum"))
        {
            ocean.ResetInitialSpectrum(&renderer);
        }

        if (ImGui::TreeNodeEx("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushItemWidth(CalculateOceanControlItemWidth());
            configChanged |= DrawOceanSettingsControls(renderer, ocean);
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Wind", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushItemWidth(CalculateOceanControlItemWidth());
            configChanged |= DrawOceanSceneControls(renderer, ocean);
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushItemWidth(CalculateOceanControlItemWidth());
            // Deliberately NOT folded into configChanged. This section is pure shader constants and
            // applies itself with SetRenderConfig (a plain assignment). Feeding it to configChanged
            // sent every drag tick through ApplyConfig -> ResetGpuResources, which tears down and
            // recreates EVERY simulation texture and rebuilds the spectrum on the CPU — a full
            // re-init per mouse move, which is what made dragging any of these sliders stall.
            DrawOceanRenderControls(ocean);
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Spectrum inputs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushItemWidth(CalculateOceanControlItemWidth());
            configChanged |= DrawOceanInputControls(renderer, ocean, localEqualizerBackups);
            ImGui::PopItemWidth();
            ImGui::TreePop();
        }
        return configChanged;
    }
}

void OceanControlsWindow::RefreshConfigFiles(const OceanSimulation& ocean)
{
    const std::wstring previousSelection =
        selectedConfigIndex_ >= 0 && selectedConfigIndex_ < static_cast<int>(configPaths_.size())
            ? configPaths_[static_cast<size_t>(selectedConfigIndex_)]
            : std::wstring{};

    configPaths_.clear();
    const std::filesystem::path configDir = L"data/ocean";
    std::error_code ec;
    if (std::filesystem::exists(configDir, ec))
    {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(configDir, ec))
        {
            if (ec)
            {
                break;
            }
            if (entry.is_regular_file(ec) && entry.path().extension() == L".json")
            {
                configPaths_.push_back(entry.path().wstring());
            }
        }
    }

    std::sort(configPaths_.begin(), configPaths_.end());

    const auto selectPath = [this](const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
        for (size_t i = 0; i < configPaths_.size(); ++i)
        {
            if (std::filesystem::path(configPaths_[i]).lexically_normal() == normalized)
            {
                selectedConfigIndex_ = static_cast<int>(i);
                return true;
            }
        }
        return false;
    };

    selectedConfigIndex_ = -1;
    if (!selectPath(previousSelection))
    {
        selectPath(ocean.GetConfigPath());
    }
    if (selectedConfigIndex_ < 0 && !configPaths_.empty())
    {
        selectedConfigIndex_ = 0;
    }

    configFilesInitialized_ = true;
}

bool OceanControlsWindow::LoadConfigAtIndex(Renderer& renderer, OceanSimulation& ocean, int configIndex)
{
    if (configIndex < 0 || configIndex >= static_cast<int>(configPaths_.size()))
    {
        configStatus_ = "Load failed";
        return false;
    }

    const int previousSelection = selectedConfigIndex_;
    selectedConfigIndex_ = configIndex;
    const std::wstring& path = configPaths_[static_cast<size_t>(configIndex)];
    if (ocean.LoadConfig(&renderer, path))
    {
        configDirty_ = false;
        localEqualizerBackups_.clear();
        configStatus_ = "Loaded " + FileNameLabel(path);
        return true;
    }

    selectedConfigIndex_ = previousSelection;
    configStatus_ = "Load failed";
    return false;
}

void OceanControlsWindow::DrawConfigControls(Renderer& renderer, OceanSimulation& ocean)
{
    if (!configFilesInitialized_)
    {
        RefreshConfigFiles(ocean);
    }

    if (!ImGui::TreeNodeEx("Config", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }

    const bool hasSelection =
        selectedConfigIndex_ >= 0 && selectedConfigIndex_ < static_cast<int>(configPaths_.size());
    const std::string selectedLabel = hasSelection
        ? FileNameLabel(configPaths_[static_cast<size_t>(selectedConfigIndex_)])
        : std::string("None");

    bool openLoadConfirm = false;
    if (ImGui::BeginCombo("Config file", selectedLabel.c_str()))
    {
        for (size_t i = 0; i < configPaths_.size(); ++i)
        {
            const std::string label = FileNameLabel(configPaths_[i]);
            const bool selected = static_cast<int>(i) == selectedConfigIndex_;
            if (ImGui::Selectable(label.c_str(), selected))
            {
                if (!selected)
                {
                    if (configDirty_)
                    {
                        pendingConfigLoadIndex_ = static_cast<int>(i);
                        const ImVec2 mousePos = ImGui::GetMousePos();
                        pendingConfigLoadPopupX_ = mousePos.x;
                        pendingConfigLoadPopupY_ = mousePos.y;
                        openLoadConfirm = true;
                    }
                    else
                    {
                        LoadConfigAtIndex(renderer, ocean, static_cast<int>(i));
                    }
                }
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Refresh"))
    {
        RefreshConfigFiles(ocean);
        const bool hasRefreshedSelection =
            selectedConfigIndex_ >= 0 && selectedConfigIndex_ < static_cast<int>(configPaths_.size());
        if (hasRefreshedSelection)
        {
            if (configDirty_)
            {
                pendingConfigLoadIndex_ = selectedConfigIndex_;
                const ImVec2 mousePos = ImGui::GetMousePos();
                pendingConfigLoadPopupX_ = mousePos.x;
                pendingConfigLoadPopupY_ = mousePos.y;
                openLoadConfirm = true;
            }
            else
            {
                LoadConfigAtIndex(renderer, ocean, selectedConfigIndex_);
            }
        }
    }
    if (openLoadConfirm)
    {
        ImGui::OpenPopup("Load Ocean Config?###OceanConfigLoadConfirm");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!hasSelection);
    if (ImGui::Button("Save"))
    {
        const std::wstring& path = configPaths_[static_cast<size_t>(selectedConfigIndex_)];
        if (ocean.SaveConfig(path))
        {
            configDirty_ = false;
            configStatus_ = "Saved " + FileNameLabel(path);
            RefreshConfigFiles(ocean);
        }
        else
        {
            configStatus_ = "Save failed";
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Save as##OceanConfigSaveAsButton"))
    {
        const std::string defaultName = hasSelection
            ? FileNameLabel(configPaths_[static_cast<size_t>(selectedConfigIndex_)])
            : std::string("default.json");
        std::snprintf(saveAsName_, sizeof(saveAsName_), "%s", defaultName.c_str());
        ImGui::OpenPopup("Save Ocean Config As###OceanConfigSaveAsPopup");
    }

    if (ImGui::BeginPopupModal("Save Ocean Config As###OceanConfigSaveAsPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Filename##OceanConfigSaveAsName", saveAsName_, IM_ARRAYSIZE(saveAsName_));
        if (ImGui::Button("Save##OceanConfigSaveAsConfirm"))
        {
            const std::wstring path = BuildConfigPathFromName(saveAsName_);
            if (path.empty())
            {
                configStatus_ = "Save failed";
            }
            else if (ocean.SaveConfig(path))
            {
                configDirty_ = false;
                configStatus_ = "Saved " + FileNameLabel(path);
                RefreshConfigFiles(ocean);
                const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
                for (size_t i = 0; i < configPaths_.size(); ++i)
                {
                    if (std::filesystem::path(configPaths_[i]).lexically_normal() == normalized)
                    {
                        selectedConfigIndex_ = static_cast<int>(i);
                        break;
                    }
                }
                ImGui::CloseCurrentPopup();
            }
            else
            {
                configStatus_ = "Save failed";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##OceanConfigSaveAsCancel"))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    constexpr float kLoadConfirmContentWidth = 660.0f;
    if (openLoadConfirm)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 popupPos(pendingConfigLoadPopupX_ + 12.0f, pendingConfigLoadPopupY_ + 12.0f);
        if (viewport)
        {
            const float minX = viewport->WorkPos.x + 8.0f;
            const float minY = viewport->WorkPos.y + 8.0f;
            const float maxX = viewport->WorkPos.x + viewport->WorkSize.x - kLoadConfirmContentWidth - 40.0f;
            const float maxY = viewport->WorkPos.y + viewport->WorkSize.y - 160.0f;
            popupPos.x = std::clamp(popupPos.x, minX, std::max(minX, maxX));
            popupPos.y = std::clamp(popupPos.y, minY, std::max(minY, maxY));
        }
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing);
        ImGui::SetNextWindowContentSize(ImVec2(kLoadConfirmContentWidth, 0.0f));
    }

    if (ImGui::BeginPopupModal("Load Ocean Config?###OceanConfigLoadConfirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const bool hasPending =
            pendingConfigLoadIndex_ >= 0 && pendingConfigLoadIndex_ < static_cast<int>(configPaths_.size());
        const std::string pendingName = hasPending
            ? FileNameLabel(configPaths_[static_cast<size_t>(pendingConfigLoadIndex_)])
            : std::string("selected config");
        ImGui::TextWrapped("Current ocean config has unsaved changes.");
        ImGui::TextWrapped("Load %s and discard those changes?", pendingName.c_str());
        if (ImGui::Button("Load##OceanConfigLoadConfirmButton"))
        {
            if (hasPending)
            {
                LoadConfigAtIndex(renderer, ocean, pendingConfigLoadIndex_);
            }
            pendingConfigLoadIndex_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##OceanConfigLoadCancelButton"))
        {
            pendingConfigLoadIndex_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!configStatus_.empty())
    {
        ImGui::TextDisabled("%s", configStatus_.c_str());
    }
    if (configDirty_)
    {
        ImGui::TextDisabled("Unsaved changes");
    }

    ImGui::TreePop();
}

void OceanControlsWindow::Draw(Renderer& renderer)
{
    if (!open_)
    {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(520.0f, 720.0f), ImGuiCond_FirstUseEver);

    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoCollapse |
        (maximize_.maximized ? ImGuiWindowFlags_NoMove : 0);
    if (ImGui::Begin("Ocean Controls [F7]###OceanControls", &open_, windowFlags))
    {
        ui::HandleWindowTitleDoubleClickMaximize(maximize_);
        OceanSimulation* ocean = Systems::GetOceanSimulation();
        if (!ocean)
        {
            ImGui::TextDisabled("No ocean simulation.");
        }
        else
        {
            DrawConfigControls(renderer, *ocean);
            ImGui::Separator();
            if (DrawOceanControlsContent(renderer, *ocean, localEqualizerBackups_))
            {
                configDirty_ = true;
            }
        }
    }
    ImGui::End();
}
