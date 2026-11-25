#pragma once

#include "app/harness/Harness.h"

#include <string>

namespace harness
{
// "--intent=<phrase>": quit as soon as the editor has logged its verdict, or
// when the model has had long enough. The wait is open-ended rather than a fixed
// delay because inference takes whatever it takes.
class IntentHarness final : public Harness
{
public:
    const char* Name() const override { return "intent"; }
    bool Wanted() const override { return true; } // the verdict or the timeout of a phrase this run may not have asked
    void OnFrame(FrameContext& ctx) override;

private:
    bool started_ = false;
    double start_ = 0.0;
};

// The editor's half of "--intent" / "--chat" / "--then" (architecture plan R3.3f): what the harness needs of the editor --
// its model's state and its command box -- through this narrow interface. EditorController::Draw implements it over the
// frame's action context and calls EditorIntentFrame / EditorChatFrame where the harness's blocks used to stand (a
// command runs there); the harness's state -- warmups, passes, the --then latch -- is kept here, not in the editor.
class EditorScriptHost
{
public:
    virtual ~EditorScriptHost() = default;
    virtual bool ModelWarmed() const = 0;  // the model's own warmup has been sent
    virtual bool ModelBusy() const = 0;    // a request in flight, in the foreground or in the background
    virtual void BeginIntent(const std::string& phrase) = 0;
    // Polls the pass in flight; true once it is over, its verdict in `verdict`. `run` presses Run (--intent-run).
    virtual bool PollIntent(bool run, std::string& verdict) = 0;
    virtual void ShowCommandBar() = 0;
    virtual void SubmitPhrase(const std::string& phrase) = 0;
    virtual bool PreviewAwaitsRun() const = 0;
    virtual void RunPendingPreview() = 0;
    virtual std::string LevelPath() const = 0; // the document's level, as the log names it
};
void EditorIntentFrame(EditorScriptHost& host); // once an editor frame: --intent's passes
void EditorChatFrame(EditorScriptHost& host);   // once an editor frame: --chat, Run for --then's commands, --then
}
