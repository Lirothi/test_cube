#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "editor/EditorContext.h"
#include "editor/intent/EditorIntentResolver.h"
#include "editor/intent/EnvironmentSettings.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "app/scene/TimeOfDay.h"              // T4: setTime / setDate
#include "app/scene/TimeOfDaySettingsJson.h"

// R10.3: the editor's actions -- the level's environment: a setting by its path, the time of day and the date.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

// Works out what a setting BECOMES: an absolute `value`, or `scale` times what it is now.
// Exactly one of the two, because "make the fog thicker" and "set the fog to 0.02" are
// different requests and guessing between them silently is how a look gets wrecked.
bool ComputeEnvironmentValue(const envsettings::Setting& setting,
    const nlohmann::json& params,
    nlohmann::json& outValue,
    std::string& outError)
{
    const auto valueIt = params.find("value");
    const auto scaleIt = params.find("scale");
    const bool hasValue = valueIt != params.end() && !valueIt->is_null();
    const bool hasScale = scaleIt != params.end() && scaleIt->is_number();

    if (hasValue == hasScale)
    {
        outError = hasValue
            ? "give either 'value' or 'scale' for " + setting.path + ", not both"
            : "setEnvironment needs a 'value' (the new setting) or a 'scale' (times its "
              "current value) for " + setting.path;
        return false;
    }

    if (hasScale)
    {
        if (setting.kind != envsettings::ValueKind::Number)
        {
            outError = "'scale' only means something for a number; " + setting.path +
                " is " + envsettings::KindName(setting.kind);
            return false;
        }
        outValue = setting.current.get<double>() * scaleIt->get<double>();
        return true;
    }

    // The declared kind is the contract. A boolean setting handed 1 would otherwise become
    // a number in the level file and quietly stop being read.
    switch (setting.kind)
    {
    case envsettings::ValueKind::Number:
        if (!valueIt->is_number())
        {
            outError = setting.path + " is a number";
            return false;
        }
        break;
    case envsettings::ValueKind::Bool:
        if (!valueIt->is_boolean())
        {
            outError = setting.path + " is true/false";
            return false;
        }
        break;
    case envsettings::ValueKind::String:
        if (!valueIt->is_string())
        {
            outError = setting.path + " is text";
            return false;
        }
        break;
    case envsettings::ValueKind::Vec3:
        if (!valueIt->is_array() || valueIt->size() != 3)
        {
            outError = setting.path + " is three numbers [x, y, z]";
            return false;
        }
        break;
    }
    outValue = *valueIt;
    return true;
}

namespace editoractions
{
    // ---------------------------------------------------------- setEnvironment

    // The look of the world -- fog, sun, wind, water, exposure, grading. It goes through
    // EditEnvironmentCommand and nothing else, which is the entire point: the engine's
    // `--set` namespace could reach the same knobs in one line, but it writes into runtime
    // state, so the edit would not be in the undo stack, would not be in the document, and
    // would vanish on the next load. An edit the user cannot take back is worse than one
    // they cannot make.
    std::unique_ptr<EditorCommand> BuildSetEnvironment(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;

        const std::vector<envsettings::Setting> settings = envsettings::Enumerate(ctx.document);
        const envsettings::Setting* setting = envsettings::Find(settings, intent.target.setting);
        if (!setting)
        {
            outStatus = intent.target.setting.empty()
                ? "setEnvironment needs a setting to change"
                : "No environment setting called '" + intent.target.setting + "'";
            return nullptr;
        }

        const EditorObject* entity = nullptr;
        for (const EditorObject& candidate : ctx.document.Environment())
        {
            if (candidate.id.value == setting->owner.value)
            {
                entity = &candidate;
                break;
            }
        }
        if (!entity)
        {
            outStatus = "The entity holding '" + setting->path + "' is gone";
            return nullptr;
        }

        nlohmann::json newValue;
        if (!ComputeEnvironmentValue(*setting, intent.params, newValue, outStatus))
        {
            return nullptr;
        }

        nlohmann::json after = entity->properties;
        if (!envsettings::Write(after, setting->subPath, newValue))
        {
            outStatus = "Cannot write '" + setting->path + "'";
            return nullptr;
        }
        if (after == entity->properties)
        {
            outStatus = setting->path + " is already " + envsettings::ToText(newValue);
            return nullptr;
        }

        outStatus = setting->path + ": " + envsettings::ToText(setting->current) +
            " -> " + envsettings::ToText(newValue);
        return std::make_unique<EditEnvironmentCommand>(
            entity->id, entity->properties, std::move(after), "Set " + setting->path);
    }

    // ---------------------------------------------------------- setTime / setDate

    // Time-of-day plan T4. Both are edits of the Time of Day object's properties -- the same
    // EditEnvironmentCommand the Inspector's slider issues, so the command bar, MCP and the slider are
    // one road: one undo entry, the document dirty, the scene re-applied by EnvironmentRuntime. The
    // build and the preview share one computation, so the preview shows the clock the run will set.
    struct TimeOfDayEdit
    {
        const EditorObject* entity = nullptr;
        nlohmann::json after;
        std::string summary;
    };

    bool ComputeTimeOfDayEdit(const EditorContext& ctx, const EditorIntent& intent, bool date,
        TimeOfDayEdit& out, std::string& outError)
    {
        for (const EditorObject& candidate : ctx.document.Environment())
        {
            if (candidate.type == "timeOfDay") { out.entity = &candidate; break; }
        }
        if (!out.entity)
        {
            outError = "This level has no Time of Day (Add > Light > Time of Day)";
            return false;
        }
        TimeOfDaySettings settings{};
        DirectionalLight sun = TimeOfDaySettingsJson::DefaultSun();
        TimeOfDaySettingsJson::ApplyOverrides(out.entity->properties, settings, sun);
        out.after = out.entity->properties;
        if (!date)
        {
            const nlohmann::json spec = intent.params.value("time", nlohmann::json());
            if (!spec.is_string())
            {
                outError = "setTime needs params.time, e.g. \"18:30\", \"sunset\" or \"+2h\"";
                return false;
            }
            float hours = 0.0f;
            if (!ResolveTimeOfDaySpec(spec.get<std::string>(), settings, hours, outError))
            {
                return false;
            }
            out.after["time"] = hours;
            out.summary = "time of day: " + FormatTimeOfDay(settings.timeHours) + " -> " + FormatTimeOfDay(hours);
            return true;
        }
        const nlohmann::json spec = intent.params.value("date", nlohmann::json());
        int year = 0, month = 0, day = 0;
        if (!spec.is_string() || !ParseTimeOfDayDate(spec.get<std::string>(), year, month, day, outError))
        {
            if (outError.empty()) { outError = "setDate needs params.date, e.g. \"2026-06-21\""; }
            return false;
        }
        // A date means something only to the real sun and moon; switching them on is what asking for
        // one says, and the summary says so.
        out.after["date"] = nlohmann::json::array({ year, month, day });
        out.after["realPositions"] = true;
        char text[64];
        std::snprintf(text, sizeof(text), "date: %04d-%02d-%02d -> %04d-%02d-%02d", settings.year, settings.month,
                      settings.day, year, month, day);
        out.summary = text;
        if (!settings.realPositions) { out.summary += " (real sun and moon positions on)"; }
        return true;
    }

    std::unique_ptr<EditorCommand> BuildTimeOfDayEdit(const EditorActionContext& actionCtx,
        const EditorIntent& intent, bool date, std::string& outStatus)
    {
        TimeOfDayEdit edit;
        if (!ComputeTimeOfDayEdit(actionCtx.editor, intent, date, edit, outStatus))
        {
            return nullptr;
        }
        if (edit.after == edit.entity->properties)
        {
            outStatus = "Already so -- " + edit.summary;
            return nullptr;
        }
        outStatus = edit.summary;
        return std::make_unique<EditEnvironmentCommand>(edit.entity->id, edit.entity->properties,
            std::move(edit.after), date ? "Set Date" : "Set Time of Day");
    }

    std::unique_ptr<EditorCommand> BuildSetTime(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&, const EditorIntent& intent, std::string& outStatus)
    {
        return BuildTimeOfDayEdit(actionCtx, intent, false, outStatus);
    }

    std::unique_ptr<EditorCommand> BuildSetDate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>&, const EditorIntent& intent, std::string& outStatus)
    {
        return BuildTimeOfDayEdit(actionCtx, intent, true, outStatus);
    }

    void RefineTimeOfDayPreview(const EditorActionContext& actionCtx, const EditorIntent& intent,
        EditorIntentPreview& preview, bool date)
    {
        TimeOfDayEdit edit;
        std::string error;
        if (!ComputeTimeOfDayEdit(actionCtx.editor, intent, date, edit, error))
        {
            preview.executable = false;
            preview.problem = error;
            return;
        }
        preview.summary = edit.summary;
    }

    void RefineSetTime(const EditorActionContext& actionCtx, const EditorIntent& intent, EditorIntentPreview& preview)
    {
        RefineTimeOfDayPreview(actionCtx, intent, preview, false);
    }

    void RefineSetDate(const EditorActionContext& actionCtx, const EditorIntent& intent, EditorIntentPreview& preview)
    {
        RefineTimeOfDayPreview(actionCtx, intent, preview, true);
    }
}

#endif // WITH_EDITOR
