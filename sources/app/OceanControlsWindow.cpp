#include "app/OceanControlsWindow.h"

#include <algorithm>
#include <memory>

#include "app/Systems.h"
#include "core/math/Math.h"
#include "imgui.h"
#include "ocean/OceanSimulation.h"
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

    const char* SpectrumModelLabel(SpectrumParams::EnergySpectrumModel value)
    {
        const int index = static_cast<int>(value);
        if (index >= 0 && index < IM_ARRAYSIZE(kSpectrumModelLabels))
        {
            return kSpectrumModelLabels[index];
        }
        return "Unknown";
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

    void DrawOceanSettingsControls(Renderer& renderer, OceanSimulation& ocean)
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

        if (domainMode == OceanSimulationSettings::CascadeDomainsMode::Auto)
        {
            float simulationScale = edited.GetSimulationScale();
            if (ImGui::DragFloat("Simulation scale", &simulationScale, 1.0f, 1.0f, 10000.0f, "%.1f"))
            {
                edited.SetSimulationScale(std::max(1.0f, simulationScale));
                settingsChanged = true;
            }
        }
        else
        {
            Math::float4 manualScales = edited.GetManualLengthScales();
            if (ImGui::DragFloat4("Length scales", &manualScales.x, 0.1f, 1.0f, 10000.0f, "%.2f"))
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
        }

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

        if (settingsChanged)
        {
            ocean.SetSettings(&renderer, edited);
        }
    }

    void DrawOceanSceneControls(Renderer& renderer, OceanSimulation& ocean)
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
    }

    void DrawOceanInputControls(Renderer& renderer, OceanSimulation& ocean)
    {
        OceanSimulationInputsProvider provider = ocean.GetInputsProvider();
        bool providerChanged = false;

        OceanSimulationInputsProvider::InputsProviderMode mode = provider.GetMode();
        if (ImGui::BeginCombo("Input mode", InputModeLabel(mode)))
        {
            for (const InputModeOption& option : kInputModeOptions)
            {
                const bool selected = option.value == mode;
                if (ImGui::Selectable(option.label, selected))
                {
                    provider.SetMode(option.value);
                    mode = option.value;
                    providerChanged = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        float timeScale = provider.GetTimeScale();
        if (ImGui::DragFloat("Time scale", &timeScale, 0.01f, 0.0f, 20.0f, "%.3f"))
        {
            provider.SetTimeScale(std::max(0.0f, timeScale));
            providerChanged = true;
        }

        float depth = provider.GetDepth();
        if (ImGui::DragFloat("Water depth", &depth, 1.0f, 0.1f, 10000.0f, "%.1f"))
        {
            provider.SetDepth(std::max(0.1f, depth));
            providerChanged = true;
        }

        auto swellPreset = provider.GetSwellPreset()
            ? std::make_shared<SwellPreset>(*provider.GetSwellPreset())
            : std::make_shared<SwellPreset>();
        if (ImGui::TreeNodeEx("Swell", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool swellChanged = false;
            SpectrumParams spectrum = swellPreset->GetSpectrum();
            swellChanged |= DrawSpectrumParams("SwellSpectrum", spectrum);
            float referenceHeight = swellPreset->GetReferenceWaveHeight();
            swellChanged |= ImGui::DragFloat("Reference wave height", &referenceHeight, 0.01f, 0.0f, 20.0f, "%.2f");
            if (swellChanged)
            {
                swellPreset->SetSpectrum(spectrum);
                swellPreset->SetReferenceWaveHeight(std::max(0.0f, referenceHeight));
                provider.SetSwellPreset(swellPreset);
                providerChanged = true;
            }
            ImGui::TreePop();
        }

        auto localPreset = provider.GetLocalWavesPreset()
            ? std::make_shared<LocalWavesPreset>(*provider.GetLocalWavesPreset())
            : std::make_shared<LocalWavesPreset>();
        const bool fixedMode = mode == OceanSimulationInputsProvider::InputsProviderMode::Fixed;
        ImGui::BeginDisabled(!fixedMode);
        if (ImGui::TreeNodeEx("Local waves", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool localChanged = false;
            SpectrumParams spectrum = localPreset->GetSpectrum();
            localChanged |= DrawSpectrumParams("LocalSpectrum", spectrum);
            float referenceHeight = localPreset->GetReferenceWaveHeight();
            localChanged |= ImGui::DragFloat("Reference wave height", &referenceHeight, 0.01f, 0.0f, 20.0f, "%.2f");
            float chop = localPreset->GetChop();
            localChanged |= ImGui::DragFloat("Chop", &chop, 0.01f, 0.0f, 5.0f, "%.2f");

            if (ImGui::TreeNodeEx("Foam", ImGuiTreeNodeFlags_DefaultOpen))
            {
                FoamParams foam = localPreset->GetFoam();
                if (DrawFoamParams(foam))
                {
                    localPreset->SetFoam(foam);
                    localChanged = true;
                }
                ImGui::TreePop();
            }

            if (localChanged)
            {
                localPreset->SetSpectrum(spectrum);
                localPreset->SetReferenceWaveHeight(std::max(0.0f, referenceHeight));
                localPreset->SetChop(std::max(0.0f, chop));
                provider.SetLocalWavesPreset(localPreset);
                providerChanged = true;
            }
            ImGui::TreePop();
        }
        ImGui::EndDisabled();

        if (providerChanged)
        {
            ocean.SetInputsProvider(&renderer, provider);
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
    }

    void DrawOceanControlsContent(Renderer& renderer)
    {
        OceanSimulation* ocean = Systems::GetOceanSimulation();
        if (!ocean)
        {
            ImGui::TextDisabled("No ocean simulation.");
            return;
        }

        if (ImGui::Button("Reset initial spectrum"))
        {
            ocean->ResetInitialSpectrum(&renderer);
        }

        if (ImGui::TreeNodeEx("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawOceanSettingsControls(renderer, *ocean);
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Wind", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawOceanSceneControls(renderer, *ocean);
            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Spectrum inputs", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawOceanInputControls(renderer, *ocean);
            ImGui::TreePop();
        }
    }
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
        DrawOceanControlsContent(renderer);
    }
    ImGui::End();
}
