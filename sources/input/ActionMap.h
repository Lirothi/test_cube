#pragma once
#include <string>
#include "third_party/robin_hood.h"
#include <vector>
#include <cstdint>

class InputManager;

class ActionMap {
public:
    // Load a JSON schema. Returns false if reading/parsing failed.
    bool LoadFromJsonFile(const std::wstring& path);

    // Queries (provide the InputManager each frame)
    bool  IsActionDown(const std::string& name, const InputManager& input) const;
    bool  WasActionPressed(const std::string& name, const InputManager& input) const;
    bool  WasActionReleased(const std::string& name, const InputManager& input) const;
    float GetAxis(const std::string& name, const InputManager& input) const;

private:
    struct Digital {
        // Returns true when any key or mouse button is active
        std::vector<int> keys;           // VK_*
        std::vector<int> mouseButtons;   // 0=Left,1=Right,2=Middle
        // Optional: modifiers in the future
    };
    struct Axis {
        // Either a key pair or a mouse axis
        std::vector<int> positiveKeys;
        std::vector<int> negativeKeys;
        // mouseAxis: 0=none, 1=X, 2=Y
        uint8_t mouseAxis = 0;
        float scale = 1.0f;
        bool invert = false;
    };
    struct Action {
        std::vector<Digital> digitals;
        std::vector<Axis>    axes;
    };

    const Action* Find(const std::string& name) const;

    // Helpers
    static int  VkFromString(const std::string& s);
    static int  MouseButtonFromString(const std::string& s);

private:
    robin_hood::unordered_map<std::string, Action> actions_;
};
