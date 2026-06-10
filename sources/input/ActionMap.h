#pragma once
#include <string>
#include "third_party/robin_hood.h"
#include <vector>
#include <cstdint>

class InputManager;

class ActionMap {
public:
    // Human-readable binding, captured verbatim from the JSON for UI overlays.
    struct BindingDesc {
        std::string action;
        std::string keys; // e.g. "F3", "Ctrl+1", "LShift/RShift", "W/Up - S/Down"
    };

    // Load a JSON schema. Returns false if reading/parsing failed.
    bool LoadFromJsonFile(const std::wstring& path);

    // Queries (provide the InputManager each frame)
    bool  IsActionDown(const std::string& name, const InputManager& input) const;
    bool  WasActionPressed(const std::string& name, const InputManager& input) const;
    bool  WasActionReleased(const std::string& name, const InputManager& input) const;
    float GetAxis(const std::string& name, const InputManager& input) const;

    // Bindings in file order, for the on-screen controls overlay.
    const std::vector<BindingDesc>& GetBindingDescs() const { return bindingDescs_; }

private:
    struct Digital {
        // Returns true when any key or mouse button is active
        std::vector<int> keys;           // VK_*
        std::vector<int> mouseButtons;   // 0=Left,1=Right,2=Middle
        bool requireAllKeys = false;
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
    std::vector<BindingDesc> bindingDescs_;
};
