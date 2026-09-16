#include "editor/intent/IntentNotes.h"
#if WITH_EDITOR

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/logging/Log.h"

namespace
{
    // Written once, when the file is created. It exists to stop the next reader -- very
    // likely an agent that treats `docs/` as authoritative -- from reading a model's guess
    // as a specification.
    const char* kHeader =
        "# Editor API requests\n"
        "\n"
        "Things the Command Bar was asked for that the editor cannot do. Each entry is\n"
        "written when the local model answers `needs_api`: it records the phrase, the\n"
        "actions the model weighed and rejected, and what it thought implementing this\n"
        "would take.\n"
        "\n"
        "**These are suggestions, not specifications.** A model wrote them from one\n"
        "designer's sentence; it has no view of the codebase beyond the action registry it\n"
        "was handed. Read an entry as evidence that somebody wanted something -- the\n"
        "valuable part -- and judge the proposed design yourself. Repeats matter more than\n"
        "any single entry: the same request three times is a feature, once is a mood.\n"
        "\n"
        "Appended by `sources/editor/intent/IntentNotes.cpp`. Safe to prune.\n";
}

namespace intentnotes
{
    std::string BuildNotePrompt(const std::string& phrase,
        const EditorIntent& refusal,
        const std::string& levelPath,
        const std::string& actionList)
    {
        std::string p;
        p += "You have just told a level designer that their request cannot be done, and you\n";
        p += "were right. Now write a short note for the engineer who maintains this editor.\n\n";
        p += "What was asked (verbatim): " + phrase + "\n";
        p += "Your reading of it: " + refusal.requested + "\n";
        p += "The API you proposed: " + refusal.proposed + "\n";
        p += "Why the existing actions did not fit: " + refusal.whyExistingDontFit + "\n";
        if (!levelPath.empty())
        {
            p += "Level in the editor: " + levelPath + "\n";
        }
        p += "\nThe actions that DO exist:\n" + actionList + "\n";
        p += "Write at most 120 words of plain prose. Cover, in this order:\n";
        p += "  1. what the designer was actually trying to achieve, in their terms;\n";
        p += "  2. the smallest action that would cover it -- name, parameters, and what it\n";
        p += "     would need from the editor that the existing actions do not;\n";
        p += "  3. anything that makes it harder than it looks, if you can see one.\n\n";
        p += "Do not restate the request. Do not apologise. Do not write JSON. If you think\n";
        p += "an existing action could actually have covered this after all, say THAT instead\n";
        p += "-- a wrong refusal is worth more to the engineer than a polished proposal.\n";
        return p;
    }

    std::string FormatNote(const std::string& phrase,
        const EditorIntent& refusal,
        const std::string& levelPath,
        const std::string& modelProse,
        const std::string& timestamp)
    {
        std::string note;
        note += "\n---\n\n";
        note += "## " + (refusal.proposed.empty() ? std::string("(no API proposed)") : refusal.proposed);
        note += "\n\n";
        note += "- **asked**: " + phrase + "\n";
        if (!refusal.requested.empty())
        {
            note += "- **read as**: " + refusal.requested + "\n";
        }
        if (!refusal.whyExistingDontFit.empty())
        {
            note += "- **rejected existing because**: " + refusal.whyExistingDontFit + "\n";
        }
        if (!levelPath.empty())
        {
            note += "- **level**: " + levelPath + "\n";
        }
        note += "- **when**: " + timestamp + "\n";

        const std::string prose = modelProse.empty()
            ? std::string("(the model did not manage a note this time)")
            : modelProse;
        note += "\n" + prose + "\n";
        return note;
    }

    bool AppendNote(const std::string& path, const std::string& note)
    {
        if (path.empty())
        {
            return false;
        }

        std::error_code ec;
        const std::filesystem::path filePath(path);
        if (filePath.has_parent_path())
        {
            std::filesystem::create_directories(filePath.parent_path(), ec);
        }
        const bool existed = std::filesystem::exists(filePath, ec);

        std::ofstream file(filePath, std::ios::app | std::ios::binary);
        if (!file)
        {
            LOG_WARNING(logging::LogCategory::Editor,
                "intent model: cannot append the API-request note to {}", path);
            return false;
        }
        if (!existed)
        {
            file << kHeader;
        }
        file << note;
        return file.good();
    }

    std::string Timestamp()
    {
        const std::time_t now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm local{};
        localtime_s(&local, &now);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min);
        return buffer;
    }
}

#endif // WITH_EDITOR
