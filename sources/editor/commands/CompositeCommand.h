#pragma once
#if WITH_EDITOR

#include <memory>
#include <string>
#include <vector>

#include "editor/commands/EditorCommand.h"

// One history entry composed from multiple editor commands. Children execute in
// order and undo in reverse. A failed child rolls back already-executed children.
class CompositeCommand : public EditorCommand
{
public:
    explicit CompositeCommand(std::string historyLabel);

    void Add(std::unique_ptr<EditorCommand> command);
    bool Empty() const { return commands_.empty(); }
    std::size_t Size() const { return commands_.size(); }

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return historyLabel_; }

private:
    std::string historyLabel_;
    std::vector<std::unique_ptr<EditorCommand>> commands_;
};

#endif // WITH_EDITOR
