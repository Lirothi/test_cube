#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/intent/EditorIntent.h"

// When the model answers `needs_api` it has just worked out something the editor cannot do
// -- and it knows, right then, exactly what was asked, which existing actions it weighed,
// and why each one failed. That reasoning is worth more than the log line it currently
// becomes: written down properly it is a note to whoever extends the editor next, which in
// this project is usually an agent reading `docs/` at the start of a session, not a person
// scrolling a session log.
//
// So a refusal produces TWO things: the short structured answer the designer sees, and a
// longer note appended to a backlog file for the implementer.
//
// THE NOTE IS A SUGGESTION, NOT AN INSTRUCTION, and the file says so in its own header.
// It is written by a model from one designer's phrase; it records what was wanted and what
// the model thought it would take. Whoever reads it decides whether any of that is right.
namespace intentnotes
{
    // The prompt for the second, UNCONSTRAINED generation. No grammar: the point of this
    // one is prose a human or an agent can act on, and a JSON muzzle would ruin it.
    std::string BuildNotePrompt(const std::string& phrase,
        const EditorIntent& refusal,
        const std::string& levelPath,
        const std::string& actionList);

    // One backlog entry, ready to append. Pure, so the gate can check its shape without a
    // model in the room.
    std::string FormatNote(const std::string& phrase,
        const EditorIntent& refusal,
        const std::string& levelPath,
        const std::string& modelProse,
        const std::string& timestamp);

    // Appends to `path`, creating it with its header the first time. Returns false when the
    // file cannot be written -- a backlog that silently fails to record is worse than none.
    bool AppendNote(const std::string& path, const std::string& note);

    // Local time as "2026-09-15 02:31".
    std::string Timestamp();
}

#endif // WITH_EDITOR
