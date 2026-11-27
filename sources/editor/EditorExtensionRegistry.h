#pragma once
#if WITH_EDITOR

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorSceneDocument.h"

struct EditorContext;
class EditorCommandStack;

// Architecture plan R10.1: where in the editor's frame a panel is drawn. Windows: with the other windows, in Order();
// AfterWindows: after them and the frame's popups (the viewport's gizmo: the mouse's capture is the windows' by then).
enum class EditorPanelSlot : unsigned char { Window, AfterWindows };

class IEditorPanel
{
public:
    virtual ~IEditorPanel() = default;

    virtual std::string_view Id() const = 0;
    virtual std::string_view Label() const = 0;
    virtual bool ShowInWindowList() const = 0;
    virtual bool IsVisible() const = 0;
    virtual void SetVisible(bool visible) = 0;
    virtual void Draw(EditorContext& ctx) = 0;
    // R10.1: the registry is the list: the frame draws and the Window menu lists every registered panel in Order() (lowest
    // first), in its Slot() -- no second list of names that a new panel must also be added to (it showed nowhere until it
    // was, twice; and a panel that went stayed named in both lists).
    virtual int Order() const = 0;
    virtual EditorPanelSlot Slot() const = 0;
};

class IEditorObjectFactory
{
public:
    virtual ~IEditorObjectFactory() = default;

    virtual std::string_view Type() const = 0;
    virtual std::string_view MenuLabel() const = 0;
    virtual bool CanBuildFromAsset(const EditorAssetRecord* sourceAsset) const = 0;
    virtual nlohmann::json BuildDefaultJson(const EditorAssetRecord* sourceAsset,
        const EditorContext& ctx,
        const AssetRegistry& registry,
        const Math::float3* worldPositionHint = nullptr) const = 0;
};

class IEditorPropertyDrawer
{
public:
    virtual ~IEditorPropertyDrawer() = default;

    virtual std::string_view Type() const = 0;
    virtual void Draw(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        EditorObject& object) const = 0;
};

class EditorLambdaPanel final : public IEditorPanel
{
public:
    using DrawFn = std::function<void(EditorContext&)>;

    EditorLambdaPanel(std::string id,
        std::string label,
        bool* visible,
        bool showInWindowList,
        DrawFn draw,
        int order,
        EditorPanelSlot slot = EditorPanelSlot::Window);

    std::string_view Id() const override { return id_; }
    std::string_view Label() const override { return label_; }
    bool ShowInWindowList() const override { return showInWindowList_; }
    bool IsVisible() const override;
    void SetVisible(bool visible) override;
    void Draw(EditorContext& ctx) override;
    int Order() const override { return order_; }
    EditorPanelSlot Slot() const override { return slot_; }

private:
    std::string id_;
    std::string label_;
    bool* visible_ = nullptr;
    bool showInWindowList_ = true;
    DrawFn draw_;
    int order_ = 0;
    EditorPanelSlot slot_ = EditorPanelSlot::Window;
};

class EditorExtensionRegistry
{
public:
    void RegisterPanel(std::unique_ptr<IEditorPanel> panel);
    void RegisterObjectFactory(std::unique_ptr<IEditorObjectFactory> factory);
    void RegisterPropertyDrawer(std::unique_ptr<IEditorPropertyDrawer> drawer);

    IEditorPanel* FindPanel(std::string_view id);
    const IEditorPanel* FindPanel(std::string_view id) const;
    const IEditorObjectFactory* FindObjectFactory(std::string_view type) const;
    const IEditorPropertyDrawer* FindPropertyDrawer(std::string_view type) const;

    // R10.1: in Order() (RegisterPanel keeps them so): what the editor's frame draws and its Window menu lists.
    const std::vector<std::unique_ptr<IEditorPanel>>& Panels() const { return panels_; }
    const std::vector<std::unique_ptr<IEditorObjectFactory>>& ObjectFactories() const { return objectFactories_; }

    static void RegisterBuiltins(EditorExtensionRegistry& registry);

private:
    std::vector<std::unique_ptr<IEditorPanel>> panels_;
    std::vector<std::unique_ptr<IEditorObjectFactory>> objectFactories_;
    std::vector<std::unique_ptr<IEditorPropertyDrawer>> propertyDrawers_;
};

#endif // WITH_EDITOR
