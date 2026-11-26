#pragma once

#include <string>

// Extract a CLI value for `key`, accepting both "key=value" and "key value" (optionally quoted
// for paths with spaces). Returns "" when the key is absent or has no value.
std::string ExtractArgValue(const char* cmd, const char* key);

// The parse of the command-line switches (architecture plan R3.2b): one function a switch, the body of the branch WinMain
// had for it, named by the switch's row of the table (app/BootSwitches.cpp). boot::ParseSwitches runs them in the table's
// order, where WinMain parsed them. A switch whose parse is still its own site (WinMain's early modes, the render knobs,
// the tools) has no function here yet.
namespace boot::parse
{
// editor
void LogWindow(const char* cmd);
void MemWindow(const char* cmd);
void DevTab(const char* cmd);
void SettingsMenu(const char* cmd);
void McpPort(const char* cmd);
void Editor(const char* cmd);
void Play(const char* cmd);
void EditMesh(const char* cmd);
void BuildDist(const char* cmd);
void ShowImport(const char* cmd);
void Inspect(const char* cmd);

// frame
void Level(const char* cmd);
void Shot(const char* cmd);
void ShotDelay(const char* cmd);
void ShotCount(const char* cmd);
void ShotStep(const char* cmd);
void ShotInterval(const char* cmd);
void CamPose(const char* cmd);
void ProfDump(const char* cmd);
void Trace(const char* cmd);
void Window(const char* cmd);
void Dlss(const char* cmd);
void Scalability(const char* cmd);
void NoHud(const char* cmd);
void Sweep(const char* cmd);

// harness
void SettingsDump(const char* cmd);
void BootDump(const char* cmd);
void ScalabilityCheck(const char* cmd);
void Intent(const char* cmd);
void IntentRun(const char* cmd);
void IntentRepeat(const char* cmd);
void IntentTimeout(const char* cmd);
void Chat(const char* cmd);

} // namespace boot::parse
