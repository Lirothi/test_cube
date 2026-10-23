@echo off
rem The app with the memory trace on (docs/memory_profiling_plan.md M4/M5): every allocation and free
rem into traces\memtrace_*.mtrace. When the app exits, memtrace.py verify checks the trace and the
rem viewer opens on it (served on 127.0.0.1; Ctrl+C in this window stops it).
rem
rem   memtrace.bat                                  wind_test, Release, until you close the window
rem   memtrace.bat --level=data/levels/demo.json    any app switch passes through as written
rem   memtrace.bat --shot-delay=20                  a fixed-length run: the shot ends it
rem   set MEMTRACE_CFG=Debug                        exact std:: frame names (Release folds them, ICF)
rem   set MEMTRACE_OPTS=all,stacks=all              the --memtrace value (default: the app's own,
rem                                                 all heaps + stacks=big); stacks=all costs ~0.5 ms
rem                                                 a frame, so add --set=frame.maxFps:60 to a long run
setlocal
cd /d "%~dp0"
if not defined MEMTRACE_CFG set "MEMTRACE_CFG=Release"
set "EXE=x64\%MEMTRACE_CFG%\test_cube.exe"
if not exist "%EXE%" (
    echo %EXE% not found: build the %MEMTRACE_CFG% configuration first.
    exit /b 1
)
set "TRACE_SWITCH=--memtrace"
if defined MEMTRACE_OPTS set "TRACE_SWITCH=--memtrace=%MEMTRACE_OPTS%"
set "LEVEL=--level=data/levels/wind_test.json"
echo(%*| findstr /c:"--level=" >nul && set "LEVEL="

echo %EXE% %LEVEL% %TRACE_SWITCH% %*
start "" /wait "%EXE%" %LEVEL% %TRACE_SWITCH% %*

python tools\memtrace.py verify
if errorlevel 1 echo VERIFY FAILED: the replay does not match what the process counted alive. Do not trust this trace.
python tools\memtrace.py view
