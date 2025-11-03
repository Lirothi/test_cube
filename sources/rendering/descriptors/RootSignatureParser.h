#pragma once
#include "rendering/descriptors/RootSignatureLayout.h"
#include <regex>
#include <sstream>
#include <cctype>

// Main parser function
inline void ParseRootSignatureFromSource(const std::string& shaderSource, RootSignatureLayout& layout)
{
    std::istringstream ss(shaderSource);
    std::string line;
    std::regex re(R"(RootSignature:\s*(.*))", std::regex::icase);
    std::smatch m;

    auto ParseVisibility = [](const std::string& vis) {
        if (vis == "all") {
            return D3D12_SHADER_VISIBILITY_ALL;
        }
        if (vis == "vertex") {
            return D3D12_SHADER_VISIBILITY_VERTEX;
        }
        if (vis == "pixel") {
            return D3D12_SHADER_VISIBILITY_PIXEL;
        }
        if (vis == "geometry") {
            return D3D12_SHADER_VISIBILITY_GEOMETRY;
        }
        if (vis == "hull") {
            return D3D12_SHADER_VISIBILITY_HULL;
        }
        if (vis == "domain") {
            return D3D12_SHADER_VISIBILITY_DOMAIN;
        }
        return D3D12_SHADER_VISIBILITY_ALL;
        };

    auto ParseTableRanges = [](const std::string& inside, std::vector<D3D12_DESCRIPTOR_RANGE>& out) {
        auto Trim = [](const std::string& str) {
            size_t first = 0;
            while (first < str.size() && std::isspace(static_cast<unsigned char>(str[first]))) { ++first; }
            if (first == str.size()) { return std::string(); }
            size_t last = str.size() - 1;
            while (last > first && std::isspace(static_cast<unsigned char>(str[last]))) { --last; }
            return str.substr(first, last - first + 1);
        };

        std::regex rangeRe(R"((CBV|SRV|UAV|SAMPLER)\(([^)]*)\))", std::regex::icase);
        auto begin = std::sregex_iterator(inside.begin(), inside.end(), rangeRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string type = (*it)[1].str();
            std::string args = (*it)[2].str();

            // Split arguments by comma while trimming whitespace
            std::vector<std::string> tokens;
            size_t pos = 0;
            while (pos < args.size()) {
                size_t comma = args.find(',', pos);
                std::string part = (comma == std::string::npos) ? args.substr(pos) : args.substr(pos, comma - pos);
                tokens.push_back(Trim(part));
                if (comma == std::string::npos) { break; }
                pos = comma + 1;
            }
            if (tokens.empty()) { continue; }

            std::regex regRe(R"(([btsu])(\d+))", std::regex::icase);
            std::smatch regMatch;
            if (!std::regex_match(tokens[0], regMatch, regRe)) { continue; }

            int regNum = std::stoi(regMatch[2].str());
            int regSpace = 0;
            UINT numDescriptors = 1;

            for (size_t idx = 1; idx < tokens.size(); ++idx) {
                const std::string& token = tokens[idx];
                if (token.empty()) { continue; }

                std::regex spaceRe(R"(space\s*=\s*(\d+))", std::regex::icase);
                std::smatch spaceMatch;
                if (std::regex_match(token, spaceMatch, spaceRe)) {
                    regSpace = std::stoi(spaceMatch[1].str());
                    continue;
                }

                std::regex countRe(R"(numDescriptors\s*=\s*(\d+))", std::regex::icase);
                std::smatch countMatch;
                if (std::regex_match(token, countMatch, countRe)) {
                    numDescriptors = static_cast<UINT>(std::stoul(countMatch[1].str()));
                }
            }

            D3D12_DESCRIPTOR_RANGE range = {};
            if (type == "CBV" || type == "cbv") {
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            }
            else if (type == "SRV" || type == "srv") {
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            }
            else if (type == "UAV" || type == "uav") {
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            }
            else if (type == "SAMPLER" || type == "sampler") {
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            }
            else {
                continue;
            }

            range.NumDescriptors = numDescriptors;
            range.BaseShaderRegister = regNum;
            range.RegisterSpace = regSpace;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            out.push_back(range);
        }
        };

    UINT tableRegister = 0;
    UINT samplerTableRegister = 0;
    auto AssignTableRegister = [&layout, &tableRegister, &samplerTableRegister]() {
        if (!layout.params.empty() && layout.params.back().type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            auto& tbl = layout.params.back();
            tbl.shaderRegister = tbl.hasSamplerRanges ? samplerTableRegister++ : tableRegister++;
        }
    };

    while (std::getline(ss, line)) {
        if (std::regex_search(line, m, re)) {
            std::string spec = m[1].str();
            size_t pos = 0;

            while (pos < spec.length()) {
                // Skip whitespace
                while (pos < spec.length() && std::isspace((unsigned char)spec[pos])) { ++pos; }
                if (pos >= spec.length()) { break; }

                // TABLE(...)
                if (spec.compare(pos, 6, "TABLE(") == 0) {
                    int depth = 1;
                    size_t start = pos + 6;
                    size_t i = start;
                    for (; i < spec.length(); ++i) {
                        if (spec[i] == '(') { ++depth; }
                        if (spec[i] == ')') { --depth; }
                        if (depth == 0) { break; }
                    }
                    if (depth == 0) {
                        std::string inside = spec.substr(start, i - start);
                        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
                        ParseTableRanges(inside, ranges);     // now also handles SAMPLER
                        layout.AddTable(ranges);              // visibility defaults to ALL
                        AssignTableRegister();
                        pos = i + 1;
                        continue;
                    }
                }

                // CONSTANTS(bN,count=M[,visibility=...])
                if (spec.compare(pos, 10, "CONSTANTS(") == 0) {
                    size_t p1 = spec.find('(', pos);
                    size_t p2 = spec.find(')', p1 + 1);
                    if (p1 != std::string::npos && p2 != std::string::npos) {
                        std::string inside = spec.substr(p1 + 1, p2 - p1 - 1);
                        std::regex constre(R"(b(\d+),\s*count\s*=\s*(\d+)(?:,visibility=([a-z]+))?)", std::regex::icase);
                        std::smatch mm;
                        if (std::regex_match(inside, mm, constre)) {
                            int regNum = std::stoi(mm[1].str());
                            int count = std::stoi(mm[2].str());
                            std::string visStr = mm[3].matched ? mm[3].str() : "all";
                            D3D12_SHADER_VISIBILITY vis = ParseVisibility(visStr);
                            layout.AddConstants(regNum, count, 0, vis);
                        }
                        pos = p2 + 1;
                        continue;
                    }
                }

                // Single descriptors: CBV/SRV/UAV/SAMPLER (support visibility and space)
                std::regex resre(R"((CBV|SRV|UAV|SAMPLER)\((b|t|u|s)(\d+)(?:,space=(\d+))?(?:,visibility=([a-z]+))?\))",
                    std::regex::icase);
                std::string tail = spec.substr(pos);
                std::smatch mm;
                if (std::regex_search(tail, mm, resre) && mm.position(0) == 0) {
                    std::string type = mm[1].str();
                    int regNum = std::stoi(mm[3].str());
                    int regSpace = mm[4].matched ? std::stoi(mm[4].str()) : 0;
                    std::string visStr = mm[5].matched ? mm[5].str() : "all";
                    D3D12_SHADER_VISIBILITY vis = ParseVisibility(visStr);

                    if (type == "CBV" || type == "cbv") { layout.AddCBV(regNum, regSpace, vis); }
                    else if (type == "SRV" || type == "srv") { layout.AddSRV(regNum, regSpace, vis); }
                    else if (type == "UAV" || type == "uav") { layout.AddUAV(regNum, regSpace, vis); }
                    else if (type == "SAMPLER" || type == "sampler") { layout.AddSampler(regNum, regSpace, vis); AssignTableRegister(); }

                    pos += mm.length(0);
                    continue;
                }

                // Not recognized—move on
                ++pos;
            }
            break; // parse only the first line containing RootSignature:
        }
    }
}