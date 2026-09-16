#include "editor/intent/EditorRepoSearch.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

namespace
{
    // Where a search may look. Weights, build output, traces and third-party trees are
    // excluded: they are enormous and none of them answers a question about how THIS engine
    // is put together.
    const char* const kRoots[] = { "sources", "shaders", "tools", "docs" };
    const char* const kExtensions[] = {
        ".h", ".hpp", ".cpp", ".hlsl", ".hlsli", ".md", ".json", ".py", ".vcxproj"
    };

    // Grouped by file, and capped PER FILE. A flat "first N matching lines" cap looks
    // reasonable and is useless: the walk is alphabetical, so sources/app/ alone exhausts
    // the budget and sources/editor/ -- where the answer usually is -- is never reached.
    // Spreading the allowance across files is what makes a broad search worth running.
    constexpr std::size_t kHitsPerFile = 3;
    constexpr std::size_t kFilesListed = 12;
    constexpr std::size_t kNamesListed = 20;
    constexpr std::size_t kMaxLine = 200;
    constexpr std::size_t kMaxReadLines = 120;
    constexpr std::size_t kMaxToolsPerTurn = 4;
    // A guard on the walk, not on the answer: a repository that grew a directory of
    // generated sources should slow the chat down, not stop the editor.
    constexpr std::size_t kMaxFilesVisited = 6000;

    std::filesystem::path RepoRoot()
    {
        std::error_code ec;
        return std::filesystem::current_path(ec);
    }

    bool HasSearchableExtension(const std::filesystem::path& path)
    {
        const std::string ext = path.extension().string();
        for (const char* candidate : kExtensions)
        {
            if (ext == candidate)
            {
                return true;
            }
        }
        return false;
    }

    std::string Relative(const std::filesystem::path& path, const std::filesystem::path& root)
    {
        std::error_code ec;
        std::string text = std::filesystem::relative(path, root, ec).generic_string();
        return ec ? path.generic_string() : text;
    }

    // Resolve a model-supplied path and prove it stays inside the repository. Refuses rather
    // than clamping: the model chose this string, and silently reinterpreting a bad one is a
    // rule nobody can reason about.
    bool InsideRepo(const std::string& relative, std::filesystem::path& outPath)
    {
        const std::filesystem::path root = RepoRoot();
        std::error_code ec;
        const std::filesystem::path full =
            std::filesystem::weakly_canonical(root / relative, ec);
        if (ec)
        {
            return false;
        }
        const std::string rootText = root.generic_string();
        const std::string fullText = full.generic_string();
        if (fullText.size() < rootText.size() ||
            fullText.compare(0, rootText.size(), rootText) != 0)
        {
            return false;
        }
        outPath = full;
        return true;
    }

    bool IsFlag(const std::string& token)
    {
        return token.size() >= 2 && token[0] == '-' &&
            token.find_first_not_of("-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") ==
                std::string::npos;
    }

    // POSIX basic regex -> the ECMAScript flavour std::regex speaks.
    //
    // `grep "water\|spline"` is how alternation is spelled without -E, and it is what a
    // model writes because it is what the world writes. ECMAScript reads `\|` as a LITERAL
    // pipe, so the pattern silently matches nothing -- and "no matches" is not an error the
    // reader can see through: three empty results once convinced this model that the engine
    // had no concept of water at all.
    std::string ToEcmaScript(std::string pattern)
    {
        if (pattern.find("\\|") == std::string::npos &&
            pattern.find("\\(") == std::string::npos)
        {
            return pattern;
        }
        static const std::pair<const char*, const char*> kEscapes[] = {
            { "\\|", "|" }, { "\\(", "(" }, { "\\)", ")" },
            { "\\{", "{" }, { "\\}", "}" }, { "\\+", "+" }, { "\\?", "?" },
        };
        for (const auto& pair : kEscapes)
        {
            std::string::size_type at = 0;
            while ((at = pattern.find(pair.first, at)) != std::string::npos)
            {
                pattern.replace(at, std::strlen(pair.first), pair.second);
                at += std::strlen(pair.second);
            }
        }
        return pattern;
    }

    // Shell-shaped grep lines, tolerated: the model writes what it has seen a thousand
    // times. `-E -i "water.*(plane|level)" sources/ docs/` yields the pattern and two
    // scopes. Taking the whole line as one regex instead is not harmless -- top-level
    // alternation makes it PARSE, so the search quietly answers a question nobody asked.
    void SplitGrepArgs(const std::string& rest,
        std::string& outPattern,
        std::vector<std::string>& outScopes)
    {
        std::vector<std::string> tokens;
        std::string token;
        for (const char ch : rest)
        {
            if (ch == ' ' || ch == '\t')
            {
                if (!token.empty()) { tokens.push_back(token); token.clear(); }
                continue;
            }
            token.push_back(ch);
        }
        if (!token.empty())
        {
            tokens.push_back(token);
        }

        std::size_t i = 0;
        while (i < tokens.size() && outPattern.empty())
        {
            if (IsFlag(tokens[i]))
            {
                ++i;
                continue;
            }
            std::string value = tokens[i];
            if (!value.empty() && (value.front() == '"' || value.front() == '\''))
            {
                const char quote = value.front();
                value.erase(0, 1);
                while (!value.empty() && value.back() != quote && i + 1 < tokens.size())
                {
                    ++i;
                    value += " " + tokens[i];
                }
                if (!value.empty() && value.back() == quote)
                {
                    value.pop_back();
                }
            }
            outPattern = value;
            ++i;
        }
        for (; i < tokens.size(); ++i)
        {
            std::string scope = tokens[i];
            while (!scope.empty() && (scope.back() == '/' || scope.back() == '\\'))
            {
                scope.pop_back();
            }
            if (!scope.empty())
            {
                outScopes.push_back(scope);
            }
        }
    }

    struct FileHits
    {
        std::string path;
        std::size_t count = 0;
        std::vector<std::string> lines;
    };

    std::string Grep(const std::string& rest)
    {
        std::string pattern;
        std::vector<std::string> scopes;
        SplitGrepArgs(rest, pattern, scopes);
        if (pattern.empty())
        {
            return "grep: no pattern";
        }

        std::regex expression;
        try
        {
            expression = std::regex(ToEcmaScript(pattern),
                std::regex::ECMAScript | std::regex::icase | std::regex::optimize);
        }
        catch (const std::regex_error& e)
        {
            return std::string("grep: bad pattern: ") + e.what();
        }

        const std::filesystem::path root = RepoRoot();
        std::vector<FileHits> perFile;
        std::size_t visited = 0;

        for (const char* rootName : kRoots)
        {
            std::error_code ec;
            const std::filesystem::path base = root / rootName;
            if (!std::filesystem::is_directory(base, ec))
            {
                continue;
            }
            for (std::filesystem::recursive_directory_iterator it(base, ec), end;
                 it != end && visited < kMaxFilesVisited; it.increment(ec))
            {
                if (ec || !it->is_regular_file(ec) || !HasSearchableExtension(it->path()))
                {
                    continue;
                }
                const std::string relative = Relative(it->path(), root);
                if (!scopes.empty())
                {
                    const bool inScope = std::any_of(scopes.begin(), scopes.end(),
                        [&relative](const std::string& scope)
                        {
                            return relative == scope ||
                                relative.compare(0, scope.size() + 1, scope + "/") == 0;
                        });
                    if (!inScope)
                    {
                        continue;
                    }
                }
                ++visited;

                std::ifstream file(it->path(), std::ios::binary);
                if (!file)
                {
                    continue;
                }
                FileHits hits;
                hits.path = relative;
                std::string line;
                std::size_t number = 0;
                while (std::getline(file, line))
                {
                    ++number;
                    if (!std::regex_search(line, expression))
                    {
                        continue;
                    }
                    ++hits.count;
                    if (hits.lines.size() < kHitsPerFile)
                    {
                        const std::size_t from = line.find_first_not_of(" \t");
                        std::string trimmed =
                            from == std::string::npos ? std::string{} : line.substr(from);
                        if (trimmed.size() > kMaxLine)
                        {
                            trimmed.resize(kMaxLine);
                        }
                        hits.lines.push_back(
                            relative + ":" + std::to_string(number) + ": " + trimmed);
                    }
                }
                if (hits.count != 0)
                {
                    perFile.push_back(std::move(hits));
                }
            }
        }

        if (perFile.empty())
        {
            std::string scopeText;
            for (const std::string& scope : scopes)
            {
                scopeText += (scopeText.empty() ? " under " : ", ") + scope;
            }
            return "(no matches" + scopeText + ")";
        }

        // Busiest files first: a term's real home usually mentions it more than its callers
        // do, and the first screen of a search is all anybody reads.
        std::sort(perFile.begin(), perFile.end(),
            [](const FileHits& a, const FileHits& b)
            {
                return a.count != b.count ? a.count > b.count : a.path < b.path;
            });

        std::size_t total = 0;
        for (const FileHits& hits : perFile)
        {
            total += hits.count;
        }
        std::string out = std::to_string(total) + " matches in " +
            std::to_string(perFile.size()) + " files.";
        for (std::size_t i = 0; i < perFile.size() && i < kFilesListed; ++i)
        {
            out += "\n--- " + perFile[i].path + " (" + std::to_string(perFile[i].count) + ")";
            for (const std::string& line : perFile[i].lines)
            {
                out += "\n" + line;
            }
            if (perFile[i].count > perFile[i].lines.size())
            {
                out += "\n    ... " +
                    std::to_string(perFile[i].count - perFile[i].lines.size()) +
                    " more in this file";
            }
        }
        if (perFile.size() > kFilesListed)
        {
            out += "\n... also matched, names only:";
            for (std::size_t i = kFilesListed;
                 i < perFile.size() && i < kFilesListed + kNamesListed; ++i)
            {
                out += " " + perFile[i].path + " (" + std::to_string(perFile[i].count) + ")";
            }
            if (perFile.size() > kFilesListed + kNamesListed)
            {
                out += " ... and " +
                    std::to_string(perFile.size() - kFilesListed - kNamesListed) + " more";
            }
        }
        return out;
    }

    std::string Read(const std::string& rest)
    {
        std::string path;
        std::size_t first = 1;
        std::size_t last = kMaxReadLines;
        {
            std::vector<std::string> tokens;
            std::string token;
            for (const char ch : rest)
            {
                if (ch == ' ' || ch == '\t')
                {
                    if (!token.empty()) { tokens.push_back(token); token.clear(); }
                    continue;
                }
                token.push_back(ch);
            }
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            if (tokens.empty())
            {
                return "read: expected  TOOL: read <path> <firstLine> <lastLine>";
            }
            path = tokens[0];
            if (tokens.size() >= 3)
            {
                first = static_cast<std::size_t>(std::max(1, std::atoi(tokens[1].c_str())));
                last = static_cast<std::size_t>(std::max(1, std::atoi(tokens[2].c_str())));
            }
        }
        if (last < first)
        {
            std::swap(first, last);
        }
        last = std::min(last, first + kMaxReadLines - 1);

        std::filesystem::path full;
        if (!InsideRepo(path, full))
        {
            return "read: '" + path + "' is not a path inside this repository";
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(full, ec))
        {
            return "read: no such file: " + path;
        }
        std::ifstream file(full, std::ios::binary);
        if (!file)
        {
            return "read: cannot open " + path;
        }
        std::string out;
        std::string line;
        std::size_t number = 0;
        while (std::getline(file, line) && number < last)
        {
            ++number;
            if (number < first)
            {
                continue;
            }
            if (line.size() > kMaxLine)
            {
                line.resize(kMaxLine);
            }
            out += std::to_string(number) + ": " + line + "\n";
        }
        return out.empty() ? "(no such lines)" : out;
    }

    std::string List(const std::string& rest)
    {
        std::filesystem::path full;
        if (!InsideRepo(rest, full))
        {
            return "ls: '" + rest + "' is not a path inside this repository";
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(full, ec))
        {
            return "ls: not a directory: " + rest;
        }
        std::vector<std::string> names;
        for (std::filesystem::directory_iterator it(full, ec), end; it != end; it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            names.push_back(it->path().filename().generic_string() +
                (it->is_directory(ec) ? "/" : ""));
        }
        std::sort(names.begin(), names.end());
        std::string out;
        for (std::size_t i = 0; i < names.size() && i < 80; ++i)
        {
            out += (i ? "\n" : "") + names[i];
        }
        if (names.size() > 80)
        {
            out += "\n... and " + std::to_string(names.size() - 80) + " more";
        }
        return out.empty() ? "(empty)" : out;
    }
}

namespace reposearch
{
    const char* ProtocolPrompt()
    {
        return
            "\nYOU CAN READ THIS ENGINE'S SOURCE. To look something up, put lines like these "
            "at the END of your answer, up to four of them:\n"
            "  TOOL: grep <regex> [path ...]   -- search; paths narrow it, e.g. sources/editor\n"
            "  TOOL: read <path> <first> <last>  -- up to 120 lines of one file\n"
            "  TOOL: ls <path>                 -- what is in a directory\n"
            "The editor runs them and sends the output back, and you answer again with what "
            "it said. Searchable: sources, shaders, tools, docs. Read only -- you cannot "
            "change, delete or run anything.\n"
            "grep groups its output BY FILE and shows the busiest first, with up to three "
            "lines each, so a broad search is worth running.\n"
            "SEARCH FOR THE NAMES OF THINGS, NOT FOR THE TOPIC. Code is named after the "
            "mechanism, not after the question: the shoreline lives under `waterLevel` and "
            "`ProbeGroundHeight`, and grepping for \"shoreline\" or \"coastline\" finds "
            "nothing. Look for identifiers, types and members -- and when a search comes back "
            "empty, that means your WORD was wrong far more often than it means the engine "
            "lacks the feature.\n"
            "Use this when the answer depends on how THIS engine actually does something. Do "
            "not use it for general questions you can already answer, and do not spend a turn "
            "confirming something you have already read: each round costs the person real "
            "seconds of waiting.";
    }

    bool RunRequestedTools(const std::string& answer, std::string& outReport)
    {
        // LINE BY LINE, matched with ^...$ anchors on each -- not one multiline regex over
        // the whole answer. `std::regex::multiline` is spelled differently across the
        // standard library versions this builds against, and a protocol reader that depends
        // on which one you compiled with is a protocol that works on one machine.
        static const std::regex call(R"(^[ \t>*-]*TOOL:[ \t]*(grep|read|ls)\b[ \t]*(.*)$)",
            std::regex::ECMAScript | std::regex::icase);

        std::string report;
        std::size_t used = 0;
        std::size_t lineStart = 0;
        while (lineStart <= answer.size())
        {
            const std::size_t lineEnd = answer.find('\n', lineStart);
            std::string line = answer.substr(lineStart,
                lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
            lineStart = lineEnd == std::string::npos ? answer.size() + 1 : lineEnd + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.pop_back();
            }

            std::smatch match;
            if (!std::regex_match(line, match, call))
            {
                continue;
            }
            if (used >= kMaxToolsPerTurn)
            {
                report += "\n\n(tool budget for this turn is " +
                    std::to_string(kMaxToolsPerTurn) + " calls; the rest were not run)";
                break;
            }
            ++used;
            std::string verb = match[1].str();
            std::transform(verb.begin(), verb.end(), verb.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string rest = match[2].str();

            std::string body;
            if (verb == "grep")      { body = Grep(rest); }
            else if (verb == "read") { body = Read(rest); }
            else                     { body = List(rest); }

            report += (report.empty() ? "" : "\n\n") + ("$ " + verb + " " + rest + "\n") + body;
        }

        if (used == 0)
        {
            return false;
        }
        outReport = std::move(report);
        return true;
    }
}

#endif // WITH_EDITOR
