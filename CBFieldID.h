#pragma once
#include <cstdint>
#include <string_view>

using CBFieldID = uint32_t;

// FNV-1a 32-bit hash for compile-time constant generation of CB field identifiers
constexpr CBFieldID ComputeCBFieldID(std::string_view s)
{
    CBFieldID hash = 2166136261u; // FNV offset basis
    for (char c : s) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u; // FNV prime
    }
    return hash;
}

// Helper macro for computing a field ID from a string literal at compile time
#define CB_FIELD_ID(str) ::ComputeCBFieldID(std::string_view(str))

