#include "editor/commands/SetRaceKeyCommand.h"
#if WITH_EDITOR

#include <utility>

#include "app/game/RaceCourse.h"
#include "editor/EditorContext.h"

SetRaceKeyCommand::SetRaceKeyCommand(EditorObjectId id, std::string key, nlohmann::json value, std::string label)
    : id_(id)
    , key_(std::move(key))
    , value_(std::move(value))
    , label_(std::move(label))
{
}

std::unique_ptr<SetRaceKeyCommand> SetRaceKeyCommand::Gate(EditorObjectId id, int gate)
{
    return std::unique_ptr<SetRaceKeyCommand>(new SetRaceKeyCommand(
        id, game::kRaceGateKey, gate > 0 ? nlohmann::json(gate) : nlohmann::json(), gate > 0 ? "Set Race Gate " + std::to_string(gate) : "Clear Race Gate"));
}

std::unique_ptr<SetRaceKeyCommand> SetRaceKeyCommand::Track(EditorObjectId id, const std::string& track)
{
    return std::unique_ptr<SetRaceKeyCommand>(new SetRaceKeyCommand(id, game::kRaceTrackKey, track.empty() ? nlohmann::json() : nlohmann::json(track),
                                                                     track.empty() ? "Race Track: the first" : "Race Track " + track));
}

std::string SetRaceKeyCommand::DocumentTrack(const EditorObject& obj)
{
    const auto it = obj.properties.find(game::kRaceTrackKey);
    return it != obj.properties.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::unique_ptr<SetRaceKeyCommand> SetRaceKeyCommand::Bot(EditorObjectId id, bool bot)
{
    return std::unique_ptr<SetRaceKeyCommand>(
        new SetRaceKeyCommand(id, game::kRaceBotKey, bot ? nlohmann::json(true) : nlohmann::json(), bot ? "Bot At The Helm" : "No Bot At The Helm"));
}

int SetRaceKeyCommand::DocumentGate(const EditorObject& obj)
{
    const auto it = obj.properties.find(game::kRaceGateKey);
    return it != obj.properties.end() && it->is_number_integer() && it->get<int>() > 0 ? it->get<int>() : 0;
}

bool SetRaceKeyCommand::DocumentBot(const EditorObject& obj)
{
    const auto it = obj.properties.find(game::kRaceBotKey);
    return it != obj.properties.end() && it->is_boolean() && it->get<bool>();
}

void SetRaceKeyCommand::Apply(EditorContext& ctx, EditorObjectId id, const std::string& key, const nlohmann::json& value)
{
    EditorObject* obj = ctx.document.Find(id);
    if (!obj) { return; }
    if (value.is_null()) { obj->properties.erase(key); }
    else { obj->properties[key] = value; }
    ctx.document.SetDirty(true);
}

bool SetRaceKeyCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj) { return false; }
    const auto it = obj->properties.find(key_);
    before_ = it != obj->properties.end() ? *it : nlohmann::json();
    Apply(ctx, id_, key_, value_);
    return true;
}

void SetRaceKeyCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, key_, before_);
}

#endif // WITH_EDITOR
