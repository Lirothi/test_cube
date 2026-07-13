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
        DrawFn draw);

    std::string_view Id() const override { return id_; }
    std::string_view Label() const override { return label_; }
    bool ShowInWindowList() const override { return showInWindowList_; }
    bool IsVisible() const override;
    void SetVisible(bool visible) override;
    void Draw(EditorContext& ctx) override;

private:
    std::string id_;
    std::string label_;
    bool* visible_ = nullptr;
    bool showInWindowList_ = true;
    DrawFn draw_;
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

    const std::vector<std::unique_ptr<IEditorPanel>>& Panels() const { return panels_; }
    const std::vector<std::unique_ptr<IEditorObjectFactory>>& ObjectFactories() const { return objectFactories_; }

    static void RegisterBuiltins(EditorExtensionRegistry& registry);

private:
    std::vector<std::unique_ptr<IEditorPanel>> panels_;
    std::vector<std::unique_ptr<IEditorObjectFactory>> objectFactories_;
    std::vector<std::unique_ptr<IEditorPropertyDrawer>> propertyDrawers_;
};

#endif // WITH_EDITOR
