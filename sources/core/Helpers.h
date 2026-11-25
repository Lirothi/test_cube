#pragma once
#include <stdexcept>
#include <chrono>
#include <vector>
#include <cmath>
#include <Windows.h>

// Utility: Throw on failure
inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        throw std::runtime_error("HRESULT failed");
    }
}

// Utility: Get time in seconds
inline double GetTimeSeconds() {
    using namespace std::chrono;
    static auto start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double>(now - start).count();
}
