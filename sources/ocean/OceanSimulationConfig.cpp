#include "ocean/OceanSimulationConfig.h"
#include "ocean/OceanRenderConfigJson.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

using nlohmann::json;

namespace
{
    using EqualizerMap = std::unordered_map<std::string, std::shared_ptr<EqualizerPreset>>;

    const json* FindMember(const json& object, const char* key)
    {
        if (!object.is_object())
        {
            return nullptr;
        }

        auto it = object.find(key);
        if (it == object.end())
        {
            return nullptr;
        }
        return &(*it);
    }

    const json* FindObject(const json& object, const char* key)
    {
        const json* member = FindMember(object, key);
        return member && member->is_object() ? member : nullptr;
    }

    const json* FindArray(const json& object, const char* key)
    {
        const json* member = FindMember(object, key);
        return member && member->is_array() ? member : nullptr;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    bool ReadBoolMember(const json& object, const char* key, bool fallback)
    {
        const json* member = FindMember(object, key);
        return member && member->is_boolean() ? member->get<bool>() : fallback;
    }

    int ReadIntMember(const json& object, const char* key, int fallback)
    {
        const json* member = FindMember(object, key);
        return member && member->is_number_integer() ? member->get<int>() : fallback;
    }

    uint32_t ReadUIntMember(const json& object, const char* key, uint32_t fallback)
    {
        const json* member = FindMember(object, key);
        if (!member || !member->is_number_unsigned())
        {
            return fallback;
        }
        return member->get<uint32_t>();
    }

    float ReadFloatMember(const json& object, const char* key, float fallback)
    {
        const json* member = FindMember(object, key);
        return member && member->is_number() ? member->get<float>() : fallback;
    }

    Math::float2 ReadFloat2(const json& value, Math::float2 fallback)
    {
        if (!value.is_array() || value.size() < 2 || !value[0].is_number() || !value[1].is_number())
        {
            return fallback;
        }
        return Math::float2(value[0].get<float>(), value[1].get<float>());
    }

    Math::float4 ReadFloat4(const json& value, Math::float4 fallback)
    {
        if (!value.is_array() || value.size() < 4 ||
            !value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number())
        {
            return fallback;
        }
        return Math::float4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
    }

    Math::float2 ReadFloat2Member(const json& object, const char* key, Math::float2 fallback)
    {
        const json* member = FindMember(object, key);
        return member ? ReadFloat2(*member, fallback) : fallback;
    }

    Math::float4 ReadFloat4Member(const json& object, const char* key, Math::float4 fallback)
    {
        const json* member = FindMember(object, key);
        return member ? ReadFloat4(*member, fallback) : fallback;
    }

    std::string ReadString(const json& value, const std::string& fallback)
    {
        return value.is_string() ? value.get<std::string>() : fallback;
    }

    bool ReadResolutionValue(const json& value, OceanSimulationSettings::ResolutionValue& out)
    {
        uint32_t resolution = 0;
        if (value.is_number_unsigned())
        {
            resolution = value.get<uint32_t>();
        }
        else if (value.is_string())
        {
            const std::string text = ToLowerAscii(value.get<std::string>());
            if (text == "six" || text == "64") { resolution = 64u; }
            else if (text == "seven" || text == "128") { resolution = 128u; }
            else if (text == "eight" || text == "256") { resolution = 256u; }
            else if (text == "nine" || text == "512") { resolution = 512u; }
        }

        switch (resolution)
        {
        case 64u: out = OceanSimulationSettings::ResolutionValue::Six; return true;
        case 128u: out = OceanSimulationSettings::ResolutionValue::Seven; return true;
        case 256u: out = OceanSimulationSettings::ResolutionValue::Eight; return true;
        case 512u: out = OceanSimulationSettings::ResolutionValue::Nine; return true;
        default: return false;
        }
    }

    bool ReadCascadeCountValue(const json& value, OceanSimulationSettings::CascadesNumberValue& out)
    {
        uint32_t count = 0;
        if (value.is_number_unsigned())
        {
            count = value.get<uint32_t>();
        }
        else if (value.is_string())
        {
            const std::string text = ToLowerAscii(value.get<std::string>());
            if (text == "two" || text == "2") { count = 2u; }
            else if (text == "three" || text == "3") { count = 3u; }
            else if (text == "four" || text == "4") { count = 4u; }
        }

        switch (count)
        {
        case 2u: out = OceanSimulationSettings::CascadesNumberValue::Two; return true;
        case 3u: out = OceanSimulationSettings::CascadesNumberValue::Three; return true;
        case 4u: out = OceanSimulationSettings::CascadesNumberValue::Four; return true;
        default: return false;
        }
    }

    bool ReadReadbackModeValue(const json& value, OceanSimulationSettings::ReadbackCascadesMode& out)
    {
        if (value.is_number_unsigned())
        {
            switch (value.get<uint32_t>())
            {
            case 0u: out = OceanSimulationSettings::ReadbackCascadesMode::None; return true;
            case 1u: out = OceanSimulationSettings::ReadbackCascadesMode::One; return true;
            case 2u: out = OceanSimulationSettings::ReadbackCascadesMode::Two; return true;
            default: return false;
            }
        }

        if (!value.is_string())
        {
            return false;
        }

        const std::string text = ToLowerAscii(value.get<std::string>());
        if (text == "none") { out = OceanSimulationSettings::ReadbackCascadesMode::None; return true; }
        if (text == "one") { out = OceanSimulationSettings::ReadbackCascadesMode::One; return true; }
        if (text == "two") { out = OceanSimulationSettings::ReadbackCascadesMode::Two; return true; }
        return false;
    }

    bool ReadDomainModeValue(const json& value, OceanSimulationSettings::CascadeDomainsMode& out)
    {
        if (!value.is_string())
        {
            return false;
        }

        const std::string text = ToLowerAscii(value.get<std::string>());
        if (text == "auto") { out = OceanSimulationSettings::CascadeDomainsMode::Auto; return true; }
        if (text == "manual") { out = OceanSimulationSettings::CascadeDomainsMode::Manual; return true; }
        return false;
    }

    bool ReadInputModeValue(const json& value, OceanSimulationInputsProvider::InputsProviderMode& out)
    {
        if (!value.is_string())
        {
            return false;
        }

        const std::string text = ToLowerAscii(value.get<std::string>());
        if (text == "scale" || text == "windscale" || text == "wind_scale")
        {
            out = OceanSimulationInputsProvider::InputsProviderMode::Scale;
            return true;
        }
        if (text == "fixed")
        {
            out = OceanSimulationInputsProvider::InputsProviderMode::Fixed;
            return true;
        }
        return false;
    }

    bool ReadEnergyModelValue(const json& value, SpectrumParams::EnergySpectrumModel& out)
    {
        if (value.is_number_integer())
        {
            switch (value.get<int>())
            {
            case 0: out = SpectrumParams::EnergySpectrumModel::PM; return true;
            case 1: out = SpectrumParams::EnergySpectrumModel::JONSWAP; return true;
            case 2: out = SpectrumParams::EnergySpectrumModel::TMA; return true;
            default: return false;
            }
        }

        if (!value.is_string())
        {
            return false;
        }

        const std::string text = ToLowerAscii(value.get<std::string>());
        if (text == "pm" || text == "pierson-moskowitz" || text == "piersonmoskowitz")
        {
            out = SpectrumParams::EnergySpectrumModel::PM;
            return true;
        }
        if (text == "jonswap")
        {
            out = SpectrumParams::EnergySpectrumModel::JONSWAP;
            return true;
        }
        if (text == "tma")
        {
            out = SpectrumParams::EnergySpectrumModel::TMA;
            return true;
        }
        return false;
    }

    bool ReadFilterTypeValue(const json& value, EqualizerPreset::FilterType& out)
    {
        if (!value.is_string())
        {
            return false;
        }

        const std::string text = ToLowerAscii(value.get<std::string>());
        if (text == "bell") { out = EqualizerPreset::FilterType::Bell; return true; }
        if (text == "highshelf" || text == "high_shelf") { out = EqualizerPreset::FilterType::Highshelf; return true; }
        if (text == "lowshelf" || text == "low_shelf") { out = EqualizerPreset::FilterType::Lowshelf; return true; }
        return false;
    }

    const char* ReadbackModeName(OceanSimulationSettings::ReadbackCascadesMode value)
    {
        switch (value)
        {
        case OceanSimulationSettings::ReadbackCascadesMode::None: return "None";
        case OceanSimulationSettings::ReadbackCascadesMode::One: return "One";
        case OceanSimulationSettings::ReadbackCascadesMode::Two: return "Two";
        default: return "None";
        }
    }

    const char* DomainModeName(OceanSimulationSettings::CascadeDomainsMode value)
    {
        switch (value)
        {
        case OceanSimulationSettings::CascadeDomainsMode::Auto: return "Auto";
        case OceanSimulationSettings::CascadeDomainsMode::Manual: return "Manual";
        default: return "Auto";
        }
    }

    const char* InputModeName(OceanSimulationInputsProvider::InputsProviderMode value)
    {
        switch (value)
        {
        case OceanSimulationInputsProvider::InputsProviderMode::Scale: return "Scale";
        case OceanSimulationInputsProvider::InputsProviderMode::Fixed: return "Fixed";
        default: return "Fixed";
        }
    }

    const char* EnergyModelName(SpectrumParams::EnergySpectrumModel value)
    {
        switch (value)
        {
        case SpectrumParams::EnergySpectrumModel::PM: return "PM";
        case SpectrumParams::EnergySpectrumModel::JONSWAP: return "JONSWAP";
        case SpectrumParams::EnergySpectrumModel::TMA: return "TMA";
        default: return "PM";
        }
    }

    const char* FilterTypeName(EqualizerPreset::FilterType value)
    {
        switch (value)
        {
        case EqualizerPreset::FilterType::Bell: return "Bell";
        case EqualizerPreset::FilterType::Highshelf: return "Highshelf";
        case EqualizerPreset::FilterType::Lowshelf: return "Lowshelf";
        default: return "Bell";
        }
    }

    SpectrumParams ReadSpectrum(const json& object, SpectrumParams spectrum)
    {
        if (const json* member = FindMember(object, "energySpectrum"))
        {
            SpectrumParams::EnergySpectrumModel value = spectrum.energySpectrum;
            if (ReadEnergyModelValue(*member, value))
            {
                spectrum.energySpectrum = value;
            }
        }

        spectrum.windSpeed = ReadFloatMember(object, "windSpeed", spectrum.windSpeed);
        spectrum.fetch = ReadFloatMember(object, "fetch", spectrum.fetch);
        spectrum.peaking = ReadFloatMember(object, "peaking", spectrum.peaking);
        spectrum.scale = ReadFloatMember(object, "scale", spectrum.scale);
        spectrum.cutoffWavelength = ReadFloatMember(object, "cutoffWavelength", spectrum.cutoffWavelength);
        spectrum.alignment = ReadFloatMember(object, "alignment", spectrum.alignment);
        spectrum.extraAlignment = ReadFloatMember(object, "extraAlignment", spectrum.extraAlignment);
        return spectrum;
    }

    FoamParams ReadFoam(const json& object, FoamParams foam)
    {
        foam.decayRate = ReadFloatMember(object, "decayRate", foam.decayRate);
        foam.coverage = ReadFloatMember(object, "coverage", foam.coverage);
        foam.density = ReadFloatMember(object, "density", foam.density);
        foam.sharpness = ReadFloatMember(object, "sharpness", foam.sharpness);
        foam.persistence = ReadFloatMember(object, "persistence", foam.persistence);
        foam.trail = ReadFloatMember(object, "trail", foam.trail);
        foam.trailTextureStrength = ReadFloatMember(object, "trailTextureStrength", foam.trailTextureStrength);
        foam.trailTextureSize = ReadFloat2Member(object, "trailTextureSize", foam.trailTextureSize);
        foam.underwater = ReadFloatMember(object, "underwater", foam.underwater);
        foam.cascadesWeights = ReadFloat4Member(object, "cascadesWeights", foam.cascadesWeights);
        return foam;
    }

    void ReadRender(const json& object, OceanRenderConfig& render)
    {
        OceanRenderConfigJson::ApplyOverrides(object, render);
    }

    std::vector<EqualizerPreset::Filter> ReadFilters(const json& object, const char* key)
    {
        std::vector<EqualizerPreset::Filter> filters;
        const json* array = FindArray(object, key);
        if (!array)
        {
            return filters;
        }

        filters.reserve(array->size());
        for (const json& item : *array)
        {
            if (!item.is_object())
            {
                continue;
            }

            EqualizerPreset::Filter filter;
            if (const json* type = FindMember(item, "type"))
            {
                EqualizerPreset::FilterType parsed = filter.type;
                if (ReadFilterTypeValue(*type, parsed))
                {
                    filter.type = parsed;
                }
            }

            filter.center = ReadFloatMember(item, "center", filter.center);
            filter.value = ReadFloatMember(item, "value", filter.value);
            filter.width = ReadFloatMember(item, "width", filter.width);
            filters.push_back(filter);
        }

        return filters;
    }

    std::shared_ptr<EqualizerPreset> ReadEqualizer(const json& object)
    {
        auto preset = std::make_shared<EqualizerPreset>();
        preset->SetScaleFilters(ReadFilters(object, "scaleFilters"));
        preset->SetChopFilters(ReadFilters(object, "chopFilters"));
        return preset;
    }

    json WriteFloat2(const Math::float2& value)
    {
        return json::array({ value.x, value.y });
    }

    json WriteFloat4(const Math::float4& value)
    {
        return json::array({ value.x, value.y, value.z, value.w });
    }

    json WriteSpectrum(const SpectrumParams& spectrum)
    {
        json out;
        out["energySpectrum"] = EnergyModelName(spectrum.energySpectrum);
        out["windSpeed"] = spectrum.windSpeed;
        out["fetch"] = spectrum.fetch;
        out["peaking"] = spectrum.peaking;
        out["scale"] = spectrum.scale;
        out["cutoffWavelength"] = spectrum.cutoffWavelength;
        out["alignment"] = spectrum.alignment;
        out["extraAlignment"] = spectrum.extraAlignment;
        return out;
    }

    json WriteFoam(const FoamParams& foam)
    {
        json out;
        out["decayRate"] = foam.decayRate;
        out["coverage"] = foam.coverage;
        out["density"] = foam.density;
        out["sharpness"] = foam.sharpness;
        out["persistence"] = foam.persistence;
        out["trail"] = foam.trail;
        out["trailTextureStrength"] = foam.trailTextureStrength;
        out["trailTextureSize"] = WriteFloat2(foam.trailTextureSize);
        out["underwater"] = foam.underwater;
        out["cascadesWeights"] = WriteFloat4(foam.cascadesWeights);
        return out;
    }

    json WriteRender(const OceanRenderConfig& render)
    {
        return OceanRenderConfigJson::ToJson(render);
    }

    json WriteFilters(const std::vector<EqualizerPreset::Filter>& filters)
    {
        json out = json::array();
        for (const EqualizerPreset::Filter& filter : filters)
        {
            json item;
            item["type"] = FilterTypeName(filter.type);
            item["center"] = filter.center;
            item["value"] = filter.value;
            item["width"] = filter.width;
            out.push_back(std::move(item));
        }
        return out;
    }

    json WriteEqualizer(const std::shared_ptr<EqualizerPreset>& preset)
    {
        const std::shared_ptr<EqualizerPreset> safe = preset ? preset : EqualizerPreset::CreateDefault();
        json out;
        out["scaleFilters"] = WriteFilters(safe->GetScaleFilters());
        out["chopFilters"] = WriteFilters(safe->GetChopFilters());
        return out;
    }

    json WriteSwellPreset(const std::shared_ptr<SwellPreset>& preset)
    {
        const std::shared_ptr<SwellPreset> safe = preset ? preset : std::make_shared<SwellPreset>();
        json out;
        out["referenceWaveHeight"] = safe->GetReferenceWaveHeight();
        out["spectrum"] = WriteSpectrum(safe->GetSpectrum());
        return out;
    }

    json WriteLocalPreset(const std::shared_ptr<LocalWavesPreset>& preset,
        const std::shared_ptr<EqualizerPreset>& defaultEqualizer)
    {
        const std::shared_ptr<LocalWavesPreset> safe = preset ? preset : std::make_shared<LocalWavesPreset>();
        json out;
        out["windForce"] = safe->GetWindForce();
        out["referenceWaveHeight"] = safe->GetReferenceWaveHeight();
        out["chop"] = safe->GetChop();
        out["spectrum"] = WriteSpectrum(safe->GetSpectrum());
        out["foam"] = WriteFoam(safe->GetFoam());
        out["equalizer"] = safe->GetEqualizer() == defaultEqualizer ? json("default") : WriteEqualizer(safe->GetEqualizer());
        return out;
    }

    json WriteSettings(const OceanSimulationSettings& settings)
    {
        json out;
        out["resolution"] = settings.GetResolution();
        out["cascadeCount"] = settings.GetCascadeCount();
        out["anisotropyLevel"] = settings.GetAnisotropyLevel();
        out["simulateFoam"] = settings.ShouldSimulateFoam();
        out["updateSpectrum"] = settings.ShouldUpdateSpectrum();
        out["readbackCascades"] = ReadbackModeName(settings.GetReadbackMode());
        out["samplingIterations"] = settings.GetSamplingIterations();

        json domains;
        domains["mode"] = DomainModeName(settings.GetDomainsMode());
        domains["simulationScale"] = settings.GetSimulationScale();
        domains["allowOverlap"] = settings.AllowOverlap();
        domains["minWavesInCascade"] = settings.GetMinWavesInCascade();
        domains["manualLengthScales"] = WriteFloat4(settings.GetManualLengthScales());
        out["domains"] = std::move(domains);
        return out;
    }

    std::shared_ptr<EqualizerPreset> CloneEqualizer(const std::shared_ptr<EqualizerPreset>& source)
    {
        if (!source)
        {
            return EqualizerPreset::CreateDefault();
        }
        return std::make_shared<EqualizerPreset>(source->GetScaleFilters(), source->GetChopFilters());
    }

    std::shared_ptr<SwellPreset> CloneSwellPreset(const std::shared_ptr<SwellPreset>& source)
    {
        auto clone = std::make_shared<SwellPreset>();
        if (source)
        {
            clone->SetSpectrum(source->GetSpectrum());
            clone->SetReferenceWaveHeight(source->GetReferenceWaveHeight());
        }
        return clone;
    }

    std::shared_ptr<LocalWavesPreset> CloneLocalPreset(const std::shared_ptr<LocalWavesPreset>& source,
        const std::shared_ptr<EqualizerPreset>& sourceDefaultEqualizer,
        const std::shared_ptr<EqualizerPreset>& clonedDefaultEqualizer)
    {
        auto clone = std::make_shared<LocalWavesPreset>();
        if (!source)
        {
            clone->SetEqualizer(clonedDefaultEqualizer);
            return clone;
        }

        clone->SetSpectrum(source->GetSpectrum());
        clone->SetFoam(source->GetFoam());
        clone->SetReferenceWaveHeight(source->GetReferenceWaveHeight());
        clone->SetChop(source->GetChop());
        clone->SetWindForce(source->GetWindForce());
        if (!source->GetEqualizer() || source->GetEqualizer() == sourceDefaultEqualizer)
        {
            clone->SetEqualizer(clonedDefaultEqualizer);
        }
        else
        {
            clone->SetEqualizer(CloneEqualizer(source->GetEqualizer()));
        }
        return clone;
    }

    std::shared_ptr<EqualizerPreset> ResolveEqualizer(const json& object,
        const EqualizerMap& equalizers,
        const std::shared_ptr<EqualizerPreset>& fallback)
    {
        const json* member = FindMember(object, "equalizer");
        if (!member)
        {
            return fallback;
        }

        if (member->is_object())
        {
            return ReadEqualizer(*member);
        }

        const std::string name = ReadString(*member, std::string{});
        if (name.empty())
        {
            return fallback;
        }

        auto it = equalizers.find(name);
        return it != equalizers.end() ? it->second : fallback;
    }

    std::shared_ptr<SwellPreset> ReadSwellPreset(const json& object)
    {
        auto preset = std::make_shared<SwellPreset>();
        const json* spectrumObject = FindObject(object, "spectrum");
        preset->SetSpectrum(ReadSpectrum(spectrumObject ? *spectrumObject : object, preset->GetSpectrum()));
        preset->SetReferenceWaveHeight(ReadFloatMember(object, "referenceWaveHeight", preset->GetReferenceWaveHeight()));
        return preset;
    }

    std::shared_ptr<LocalWavesPreset> ReadLocalPreset(const json& object,
        const EqualizerMap& equalizers,
        const std::shared_ptr<EqualizerPreset>& fallbackEqualizer)
    {
        auto preset = std::make_shared<LocalWavesPreset>();
        const json* spectrumObject = FindObject(object, "spectrum");
        const json* foamObject = FindObject(object, "foam");
        preset->SetSpectrum(ReadSpectrum(spectrumObject ? *spectrumObject : object, preset->GetSpectrum()));
        preset->SetFoam(ReadFoam(foamObject ? *foamObject : object, preset->GetFoam()));
        preset->SetReferenceWaveHeight(ReadFloatMember(object, "referenceWaveHeight", preset->GetReferenceWaveHeight()));
        preset->SetChop(ReadFloatMember(object, "chop", preset->GetChop()));
        preset->SetWindForce(ReadFloatMember(object, "windForce", preset->GetWindForce()));
        preset->SetEqualizer(ResolveEqualizer(object, equalizers, fallbackEqualizer));
        return preset;
    }

    void ReadSettings(const json& object, OceanSimulationSettings& settings)
    {
        if (const json* member = FindMember(object, "resolution"))
        {
            OceanSimulationSettings::ResolutionValue value =
                static_cast<OceanSimulationSettings::ResolutionValue>(settings.GetResolution());
            if (ReadResolutionValue(*member, value))
            {
                settings.SetResolution(value);
            }
        }

        if (const json* member = FindMember(object, "cascadeCount"))
        {
            OceanSimulationSettings::CascadesNumberValue value =
                static_cast<OceanSimulationSettings::CascadesNumberValue>(settings.GetCascadeCount());
            if (ReadCascadeCountValue(*member, value))
            {
                settings.SetCascadeCount(value);
            }
        }

        settings.SetAnisotropyLevel(ReadUIntMember(object, "anisotropyLevel", settings.GetAnisotropyLevel()));
        settings.SetSimulateFoam(ReadBoolMember(object, "simulateFoam", settings.ShouldSimulateFoam()));
        settings.SetUpdateSpectrum(ReadBoolMember(object, "updateSpectrum", settings.ShouldUpdateSpectrum()));

        if (const json* member = FindMember(object, "readbackCascades"))
        {
            OceanSimulationSettings::ReadbackCascadesMode value = settings.GetReadbackMode();
            if (ReadReadbackModeValue(*member, value))
            {
                settings.SetReadbackMode(value);
            }
        }

        settings.SetSamplingIterations(ReadUIntMember(object, "samplingIterations", settings.GetSamplingIterations()));

        const json* domainObject = FindObject(object, "domains");
        const json& domains = domainObject ? *domainObject : object;
        if (const json* member = FindMember(domains, "mode"))
        {
            OceanSimulationSettings::CascadeDomainsMode value = settings.GetDomainsMode();
            if (ReadDomainModeValue(*member, value))
            {
                settings.SetDomainsMode(value);
            }
        }
        settings.SetSimulationScale(ReadFloatMember(domains, "simulationScale", settings.GetSimulationScale()));
        settings.SetAllowOverlap(ReadBoolMember(domains, "allowOverlap", settings.AllowOverlap()));
        settings.SetMinWavesInCascade(ReadFloatMember(domains, "minWavesInCascade", settings.GetMinWavesInCascade()));
        settings.SetManualLengthScales(ReadFloat4Member(domains, "manualLengthScales", settings.GetManualLengthScales()));
    }

    EqualizerMap ReadEqualizers(const json& inputs, std::shared_ptr<EqualizerPreset>& defaultEqualizer)
    {
        EqualizerMap equalizers;

        if (const json* object = FindObject(inputs, "equalizers"))
        {
            for (auto it = object->begin(); it != object->end(); ++it)
            {
                if (it.value().is_object())
                {
                    equalizers[it.key()] = ReadEqualizer(it.value());
                }
            }
        }

        if (const json* object = FindObject(inputs, "defaultEqualizer"))
        {
            defaultEqualizer = ReadEqualizer(*object);
            equalizers["default"] = defaultEqualizer;
        }
        else if (auto it = equalizers.find("default"); it != equalizers.end())
        {
            defaultEqualizer = it->second;
        }

        return equalizers;
    }

    void ReadInputs(const json& object, OceanSimulationConfig& config)
    {
        if (const json* member = FindMember(object, "mode"))
        {
            OceanSimulationInputsProvider::InputsProviderMode value = config.inputMode;
            if (ReadInputModeValue(*member, value))
            {
                config.inputMode = value;
            }
        }

        config.timeScale = ReadFloatMember(object, "timeScale", config.timeScale);
        config.depth = ReadFloatMember(object, "depth", config.depth);

        EqualizerMap equalizers = ReadEqualizers(object, config.defaultEqualizer);

        if (const json* swell = FindObject(object, "swell"))
        {
            config.swellPreset = ReadSwellPreset(*swell);
        }

        if (const json* presets = FindArray(object, "localPresets"))
        {
            config.localPresets.clear();
            config.localPresets.reserve(presets->size());
            for (const json& presetJson : *presets)
            {
                if (presetJson.is_object())
                {
                    config.localPresets.push_back(ReadLocalPreset(presetJson, equalizers, config.defaultEqualizer));
                }
            }
        }

        if (const json* localPreset = FindObject(object, "localPreset"))
        {
            config.localPreset = ReadLocalPreset(*localPreset, equalizers, config.defaultEqualizer);
        }
        else if (!config.localPresets.empty())
        {
            const int defaultIndex = std::clamp(ReadIntMember(object, "localPresetIndex", 0), 0,
                static_cast<int>(config.localPresets.size() - 1));
            config.localPresetIndex = static_cast<size_t>(defaultIndex);
            config.localPreset = config.localPresets[static_cast<size_t>(defaultIndex)];
        }
    }
}

OceanSimulationConfig CloneOceanSimulationConfig(const OceanSimulationConfig& config)
{
    OceanSimulationConfig clone;
    clone.settings = config.settings;
    clone.render = config.render;
    clone.localWindDirectionDegrees = config.localWindDirectionDegrees;
    clone.swellDirectionDegrees = config.swellDirectionDegrees;
    clone.windForce01 = config.windForce01;
    clone.inputMode = config.inputMode;
    clone.timeScale = config.timeScale;
    clone.depth = config.depth;
    clone.localPresetIndex = config.localPresetIndex;

    clone.defaultEqualizer = CloneEqualizer(config.defaultEqualizer);
    clone.swellPreset = CloneSwellPreset(config.swellPreset);

    clone.localPresets.clear();
    clone.localPresets.reserve(config.localPresets.size());
    for (const auto& preset : config.localPresets)
    {
        clone.localPresets.push_back(CloneLocalPreset(preset, config.defaultEqualizer, clone.defaultEqualizer));
    }

    if (!clone.localPresets.empty())
    {
        clone.localPresetIndex = std::min(clone.localPresetIndex, clone.localPresets.size() - 1u);
        clone.localPreset = clone.localPresets[clone.localPresetIndex];
    }
    else
    {
        clone.localPresetIndex = 0;
        clone.localPreset = CloneLocalPreset(config.localPreset, config.defaultEqualizer, clone.defaultEqualizer);
        clone.localPresets.push_back(clone.localPreset);
    }

    return clone;
}

bool LoadOceanSimulationConfigFromFile(const std::wstring& path, OceanSimulationConfig& outConfig)
{
    std::ifstream f(path);
    if (!f)
    {
        return false;
    }

    std::stringstream ss;
    ss << f.rdbuf();

    const json root = json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
    if (root.is_discarded() || !root.is_object())
    {
        return false;
    }

    OceanSimulationConfig config;

    if (const json* settings = FindObject(root, "settings"))
    {
        ReadSettings(*settings, config.settings);
    }

    if (const json* render = FindObject(root, "render"))
    {
        ReadRender(*render, config.render);
    }

    if (const json* scene = FindObject(root, "scene"))
    {
        config.localWindDirectionDegrees = ReadFloatMember(*scene, "localWindDirectionDegrees", config.localWindDirectionDegrees);
        config.swellDirectionDegrees = ReadFloatMember(*scene, "swellDirectionDegrees", config.swellDirectionDegrees);
        config.windForce01 = ReadFloatMember(*scene, "windForce01", config.windForce01);
    }

    if (const json* inputs = FindObject(root, "inputs"))
    {
        ReadInputs(*inputs, config);
    }

    outConfig = std::move(config);
    return true;
}

bool SaveOceanSimulationConfigToFile(const std::wstring& path, const OceanSimulationConfig& config)
{
    const OceanSimulationConfig safeConfig = CloneOceanSimulationConfig(config);

    json root;
    root["settings"] = WriteSettings(safeConfig.settings);
    root["render"] = WriteRender(safeConfig.render);

    json scene;
    scene["localWindDirectionDegrees"] = safeConfig.localWindDirectionDegrees;
    scene["swellDirectionDegrees"] = safeConfig.swellDirectionDegrees;
    scene["windForce01"] = Math::Saturate(safeConfig.windForce01);
    root["scene"] = std::move(scene);

    json inputs;
    inputs["mode"] = InputModeName(safeConfig.inputMode);
    inputs["timeScale"] = safeConfig.timeScale;
    inputs["depth"] = safeConfig.depth;
    inputs["defaultEqualizer"] = WriteEqualizer(safeConfig.defaultEqualizer);
    inputs["swell"] = WriteSwellPreset(safeConfig.swellPreset);
    inputs["localPresetIndex"] = safeConfig.localPresetIndex;

    json localPresets = json::array();
    for (const auto& preset : safeConfig.localPresets)
    {
        localPresets.push_back(WriteLocalPreset(preset, safeConfig.defaultEqualizer));
    }
    inputs["localPresets"] = std::move(localPresets);
    root["inputs"] = std::move(inputs);

    const std::filesystem::path filePath(path);
    if (const std::filesystem::path parent = filePath.parent_path(); !parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return false;
        }
    }

    std::ofstream f(filePath);
    if (!f)
    {
        return false;
    }

    f << root.dump(2) << '\n';
    return static_cast<bool>(f);
}
