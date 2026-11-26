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

// R3.2c: the debug switches (app/BootParseDev.cpp).
void BarrierCmp(const char* cmdLine);
void RetireSelftest(const char* cmdLine);
void RtAudit(const char* cmdLine);
void CloudProfile(const char* cmdLine);
void CloudFormation(const char* cmdLine);
void CloudCells(const char* cmdLine);
void CloudDump(const char* cmdLine);
void VsmPageMultidraw(const char* cmdLine);
void VsmNoLocalScatter(const char* cmdLine);
void VsmPageCompaction(const char* cmdLine);
void BarrierCompileLog(const char* cmdLine);
void BarrierFlipTrace(const char* cmdLine);
void LegacyBarriers(const char* cmdLine);
void EnhancedBarriers(const char* cmdLine);
void BarrierMsgTrace(const char* cmdLine);
void BarrierCensus(const char* cmdLine);
void BarrierSyncWide(const char* cmdLine);
void BarrierCacheVerify(const char* cmdLine);
void CanonicalCheck(const char* cmdLine);
void ComputeLaneProbe(const char* cmdLine);
void AsyncEmptySubmit(const char* cmdLine);
void NoAsyncCompute(const char* cmdLine);
void TessyLodBuild(const char* cmdLine);
void TessyLodPrune(const char* cmdLine);
void TessyLodFold(const char* cmdLine);
void NoStreamline(const char* cmdLine);
void NoBindBatching(const char* cmdLine);
void AsyncPass(const char* cmdLine);
void DrCheck(const char* cmdLine);
void UploadRingCheck(const char* cmdLine);
void WaterMaskSelftest(const char* cmdLine);
void TexstreamSelftest(const char* cmdLine);
void DumpBarriers(const char* cmdLine);
void DumpSubmitOrder(const char* cmdLine);
void AsyncOrderProbe(const char* cmdLine);
void ShadowMode(const char* cmdLine);
void Dred(const char* cmdLine);
void NoD3dDebug(const char* cmdLine);
void SceneStress(const char* cmdLine);
void Gbv(const char* cmdLine);
void BootProfileBuckets(const char* cmdLine);
void WindFreeze(const char* cmdLine);
void MemReport(const char* cmdLine);
void OceanShoreSink(const char* cmdLine);
void OceanWind(const char* cmdLine);
void OceanVsDepthProbe(const char* cmdLine);
void OceanGeomFade(const char* cmdLine);
void OceanSurfSimShore(const char* cmdLine);
void OceanRunupShore(const char* cmdLine);
void OceanFoamDebug(const char* cmdLine);
void OceanSurfSim(const char* cmdLine);
void OceanSurfDebug(const char* cmdLine);
void OceanSurfPoke(const char* cmdLine);
void DlssMvec(const char* cmdLine);
void VsmRefDist(const char* cmdLine);
void VsmNormalBias(const char* cmdLine);
void VsmResident(const char* cmdLine);
void VsmSingleDraw(const char* cmdLine);
void VsmCompactArgs(const char* cmdLine);
void VsmLodBias(const char* cmdLine);
void VsmLodStride(const char* cmdLine);
void CsmTint(const char* cmdLine);
void VisReadout(const char* cmdLine);
void CsmReadout(const char* cmdLine);
void SdsmReadout(const char* cmdLine);
} // namespace boot::parse
