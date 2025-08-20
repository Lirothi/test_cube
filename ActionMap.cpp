#include "ActionMap.h"
#include "InputManager.h"
#include <fstream>
#include <sstream>

// nlohmann/json — single header
#include "json/json.hpp"
using nlohmann::json;

static std::string ToLower(std::string s) {
    for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = char(c - 'A' + 'a'); } }
    return s;
}

static bool KeyDownCompat(int vk, const InputManager& in) {
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        return in.IsKeyDown(VK_SHIFT) || in.IsKeyDown(VK_LSHIFT) || in.IsKeyDown(VK_RSHIFT);
    }
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return in.IsKeyDown(VK_CONTROL) || in.IsKeyDown(VK_LCONTROL) || in.IsKeyDown(VK_RCONTROL);
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return in.IsKeyDown(VK_MENU) || in.IsKeyDown(VK_LMENU) || in.IsKeyDown(VK_RMENU);
    }
    return in.IsKeyDown(vk);
}

static bool KeyPressedCompat(int vk, const InputManager& in) {
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        return in.WasKeyPressed(VK_SHIFT) || in.WasKeyPressed(VK_LSHIFT) || in.WasKeyPressed(VK_RSHIFT);
    }
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return in.WasKeyPressed(VK_CONTROL) || in.WasKeyPressed(VK_LCONTROL) || in.WasKeyPressed(VK_RCONTROL);
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return in.WasKeyPressed(VK_MENU) || in.WasKeyPressed(VK_LMENU) || in.WasKeyPressed(VK_RMENU);
    }
    return in.WasKeyPressed(vk);
}

static bool KeyReleasedCompat(int vk, const InputManager& in) {
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        return in.WasKeyReleased(VK_SHIFT) || in.WasKeyReleased(VK_LSHIFT) || in.WasKeyReleased(VK_RSHIFT);
    }
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return in.WasKeyReleased(VK_CONTROL) || in.WasKeyReleased(VK_LCONTROL) || in.WasKeyReleased(VK_RCONTROL);
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return in.WasKeyReleased(VK_MENU) || in.WasKeyReleased(VK_LMENU) || in.WasKeyReleased(VK_RMENU);
    }
    return in.WasKeyReleased(vk);
}

bool ActionMap::LoadFromJsonFile(const std::wstring& path) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) {
        return false;
    }

    actions_.clear();

    if (j.contains("speed")) {
        auto sp = j["speed"];
        if (sp.contains("move") && sp["move"].is_number()) {
            moveSpeed_ = sp["move"].get<float>();
        }
        if (sp.contains("sprintMultiplier") && sp["sprintMultiplier"].is_number()) {
            sprintMultiplier_ = sp["sprintMultiplier"].get<float>();
        }
    }

    if (j.contains("actions") && j["actions"].is_array()) {
        for (auto& a : j["actions"]) {
            if (!a.contains("name") || !a["name"].is_string()) {
                continue;
            }
            std::string name = a["name"].get<std::string>();
            Action act;

            // Digital bindings
            if (a.contains("keys") && a["keys"].is_array()) {
                Digital d;
                for (auto& s : a["keys"]) {
                    if (s.is_string()) {
                        int vk = VkFromString(s.get<std::string>());
                        if (vk != 0) { d.keys.push_back(vk); }
                    }
                }
                if (!d.keys.empty()) {
                    act.digitals.push_back(d);
                }
            }
            if (a.contains("mouseButton")) {
                Digital d;
                int mb = MouseButtonFromString(a["mouseButton"].get<std::string>());
                if (mb >= 0) { d.mouseButtons.push_back(mb); }
                if (!d.mouseButtons.empty()) {
                    act.digitals.push_back(d);
                }
            }
            if (a.contains("mouseButtons") && a["mouseButtons"].is_array()) {
                Digital d;
                for (auto& s : a["mouseButtons"]) {
                    if (s.is_string()) {
                        int mb = MouseButtonFromString(s.get<std::string>());
                        if (mb >= 0) { d.mouseButtons.push_back(mb); }
                    }
                }
                if (!d.mouseButtons.empty()) {
                    act.digitals.push_back(d);
                }
            }

            // Axis from key pairs
            if (a.contains("positive") || a.contains("negative")) {
                Axis ax;
                if (a.contains("positive") && a["positive"].is_array()) {
                    for (auto& s : a["positive"]) {
                        if (s.is_string()) {
                            int vk = VkFromString(s.get<std::string>());
                            if (vk != 0) { ax.positiveKeys.push_back(vk); }
                        }
                    }
                }
                if (a.contains("negative") && a["negative"].is_array()) {
                    for (auto& s : a["negative"]) {
                        if (s.is_string()) {
                            int vk = VkFromString(s.get<std::string>());
                            if (vk != 0) { ax.negativeKeys.push_back(vk); }
                        }
                    }
                }
                if (!ax.positiveKeys.empty() || !ax.negativeKeys.empty()) {
                    if (a.contains("scale") && a["scale"].is_number()) {
                        ax.scale = a["scale"].get<float>();
                    }
                    act.axes.push_back(ax);
                }
            }

            // Axis from mouse
            if (a.contains("mouseAxis") && a["mouseAxis"].is_string()) {
                Axis ax;
                std::string which = ToLower(a["mouseAxis"].get<std::string>());
                if (which == "x") { ax.mouseAxis = 1; }
                if (which == "y") { ax.mouseAxis = 2; }
                if (a.contains("sensitivity") && a["sensitivity"].is_number()) {
                    ax.scale = a["sensitivity"].get<float>();
                }
                if (a.contains("invert") && a["invert"].is_boolean()) {
                    ax.invert = a["invert"].get<bool>();
                }
                if (ax.mouseAxis != 0) {
                    act.axes.push_back(ax);
                }
            }

            actions_.emplace(name, std::move(act));
        }
    }

    return true;
}

const ActionMap::Action* ActionMap::Find(const std::string& name) const {
    auto it = actions_.find(name);
    if (it == actions_.end()) {
        return nullptr;
    }
    return &it->second;
}

// === Queries ===

bool ActionMap::IsActionDown(const std::string& name, const InputManager& input) const {
    auto a = Find(name);
    if (a == nullptr) { return false; }

    for (const auto& d : a->digitals) {
        for (int vk : d.keys) {
            if (KeyDownCompat(vk, input)) { return true; }
        }
        for (int mb : d.mouseButtons) {
            if (mb == 0 && input.IsLButtonDown()) { return true; }
            if (mb == 1 && input.IsRButtonDown()) { return true; }
            if (mb == 2 && input.IsMButtonDown()) { return true; }
        }
    }
    return false;
}

bool ActionMap::WasActionPressed(const std::string& name, const InputManager& input) const {
    auto a = Find(name);
    if (a == nullptr) { return false; }
    for (const auto& d : a->digitals) {
        for (int vk : d.keys) {
            if (KeyPressedCompat(vk, input)) { return true; }
        }
    }
    return false;
}

bool ActionMap::WasActionReleased(const std::string& name, const InputManager& input) const {
    auto a = Find(name);
    if (a == nullptr) { return false; }
    for (const auto& d : a->digitals) {
        for (int vk : d.keys) {
            if (KeyReleasedCompat(vk, input)) { return true; }
        }
    }
    return false;
}

float ActionMap::GetAxis(const std::string& name, const InputManager& input) const {
    auto a = Find(name);
    if (a == nullptr) {
        return 0.0f;
    }
    float v = 0.0f;
    for (const auto& ax : a->axes) {
        float add = 0.0f;
        if (!ax.positiveKeys.empty() || !ax.negativeKeys.empty()) {
            bool pos = false;
            bool neg = false;
            for (int k : ax.positiveKeys) { if (input.IsKeyDown(k)) { pos = true; break; } }
            for (int k : ax.negativeKeys) { if (input.IsKeyDown(k)) { neg = true; break; } }
            add = (pos ? 1.0f : 0.0f) - (neg ? 1.0f : 0.0f);
            add *= ax.scale;
        }
        if (ax.mouseAxis == 1) {
            float dx = static_cast<float>(input.MouseDeltaX());
            add += (ax.invert ? -dx : dx) * ax.scale;
        } else if (ax.mouseAxis == 2) {
            float dy = static_cast<float>(input.MouseDeltaY());
            add += (ax.invert ? -dy : dy) * ax.scale;
        }
        v += add;
    }
    return v;
}

// === VK helpers ===

static int VkFromNameLower_(const std::string& s) {
    if (s == "w") { return 'W'; }
    if (s == "a") { return 'A'; }
    if (s == "s") { return 'S'; }
    if (s == "d") { return 'D'; }
    if (s == "q") { return 'Q'; }
    if (s == "e") { return 'E'; }
    if (s == "space") { return VK_SPACE; }

    // модификаторы — в общий VK_* (WM_KEYDOWN чаще отдаёт именно их)
    if (s == "lshift" || s == "rshift" || s == "shift") { return VK_SHIFT; }
    if (s == "lctrl" || s == "rctrl" || s == "ctrl") { return VK_CONTROL; }
    if (s == "lalt" || s == "ralt" || s == "alt" || s == "lmenu" || s == "rmenu") { return VK_MENU; }

    if (s == "up") { return VK_UP; }
    if (s == "down") { return VK_DOWN; }
    if (s == "left") { return VK_LEFT; }
    if (s == "right") { return VK_RIGHT; }
    if (s == "pageup") { return VK_PRIOR; }
    if (s == "pagedown") { return VK_NEXT; }
    return 0;
}

int ActionMap::VkFromString(const std::string& s) {
    return VkFromNameLower_(ToLower(s));
}

int ActionMap::MouseButtonFromString(const std::string& s) {
    const std::string t = ToLower(s);
    if (t == "left"  || t == "lmb") { return 0; }
    if (t == "right" || t == "rmb") { return 1; }
    if (t == "middle"|| t == "mmb") { return 2; }
    return -1;
}