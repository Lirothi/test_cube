#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

class InputManager;

class ActionMap {
public:
    // Загрузка JSON схемы. Вернёт false, если не удалось прочитать/распарсить.
    bool LoadFromJsonFile(const std::wstring& path);

    // Запросы (передаём InputManager каждый кадр)
    bool  IsActionDown(const std::string& name, const InputManager& input) const;
    bool  WasActionPressed(const std::string& name, const InputManager& input) const;
    bool  WasActionReleased(const std::string& name, const InputManager& input) const;
    float GetAxis(const std::string& name, const InputManager& input) const;

    // Параметры скорости/сенс
    float MoveSpeed() const { return moveSpeed_; }
    float SprintMultiplier() const { return sprintMultiplier_; }

private:
    struct Digital {
        // любой из keys или mouseButtons активен -> true
        std::vector<int> keys;           // VK_*
        std::vector<int> mouseButtons;   // 0=Left,1=Right,2=Middle
        // (опционально) модификаторы в будущем
    };
    struct Axis {
        // либо пара клавиш, либо мышиная ось
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
    std::unordered_map<std::string, Action> actions_;
    float moveSpeed_ = 3.0f;
    float sprintMultiplier_ = 2.5f;
};