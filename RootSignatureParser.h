#pragma once
#include "RootSignatureLayout.h"
#include <regex>
#include <sstream>
#include <cctype>

// Главная функция-парсер
inline void ParseRootSignatureFromSource(const std::string& shaderSource, RootSignatureLayout& layout)
{
    std::istringstream ss(shaderSource);
    std::string line;
    std::regex re(R"(RootSignature:\s*(.*))", std::regex::icase);
    std::smatch m;

    auto ParseVisibility = [](const std::string& vis) {
        if (vis == "all")     return D3D12_SHADER_VISIBILITY_ALL;
        if (vis == "vertex")  return D3D12_SHADER_VISIBILITY_VERTEX;
        if (vis == "pixel")   return D3D12_SHADER_VISIBILITY_PIXEL;
        if (vis == "geometry")return D3D12_SHADER_VISIBILITY_GEOMETRY;
        if (vis == "hull")    return D3D12_SHADER_VISIBILITY_HULL;
        if (vis == "domain")  return D3D12_SHADER_VISIBILITY_DOMAIN;
        return D3D12_SHADER_VISIBILITY_ALL;
        };

    auto ParseTableRanges = [](const std::string& inside, std::vector<D3D12_DESCRIPTOR_RANGE>& out) {
        // Добавили SAMPLER и регистр 's'
        std::regex rangeRe(R"((CBV|SRV|UAV|SAMPLER)\((b|t|u|s)(\d+)(?:,space=(\d+))?\))", std::regex::icase);
        auto begin = std::sregex_iterator(inside.begin(), inside.end(), rangeRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string type = (*it)[1].str();
            // char regType = (*it)[2].str()[0]; // при желании можно использовать
            int regNum = std::stoi((*it)[3].str());
            int regSpace = (*it)[4].matched ? std::stoi((*it)[4].str()) : 0;

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
            range.NumDescriptors = 1;
            range.BaseShaderRegister = regNum;
            range.RegisterSpace = regSpace;
            // Важно: аппендим корректно, не "0"
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            out.push_back(range);
        }
        };

    while (std::getline(ss, line)) {
        if (std::regex_search(line, m, re)) {
            std::string spec = m[1].str();
            size_t pos = 0;

            while (pos < spec.length()) {
                // Пропуски
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
                        ParseTableRanges(inside, ranges);     // теперь тянет и SAMPLER
                        layout.AddTable(ranges);              // видимость — по умолчанию ALL
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

                // Одиночные: CBV/SRV/UAV/SAMPLER (поддержка visibility и space)
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
                    else if (type == "SAMPLER" || type == "sampler") { layout.AddSampler(regNum, regSpace, vis); }

                    pos += mm.length(0);
                    continue;
                }

                // Не распознали — двигаемся дальше
                ++pos;
            }
            break; // парсим только первую найденную строку с RootSignature:
        }
    }
}