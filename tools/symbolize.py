r"""Resolves the "module+0xoffset" frames of a log line (the device removal report, DeviceRemovalWatch.cpp) through
dbghelp: the engine's by its PDB, a system DLL by its exports. No symbol server, no downloads.

python tools/symbolize.py <exe> <log file or text> [<log file or text> ...]
    python tools/symbolize.py x64/Debug/test_cube.exe logs/session_..._debug.log    every line with frames
    python tools/symbolize.py x64/Debug/test_cube.exe "test_cube.exe+0x1c7451e win32u.dll+0x4b84"
"""
import ctypes
import ctypes.wintypes as w
import os
import re
import sys

FRAME = re.compile(r"([\w.]+\.(?:exe|dll))\+0x([0-9a-fA-F]+)")


class SymbolInfo(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG), ("Reserved", ctypes.c_ulonglong * 2), ("Index", w.ULONG),
                ("Size", w.ULONG), ("ModBase", ctypes.c_ulonglong), ("Flags", w.ULONG), ("Value", ctypes.c_ulonglong),
                ("Address", ctypes.c_ulonglong), ("Register", w.ULONG), ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG),
                ("MaxNameLen", w.ULONG), ("Name", ctypes.c_char * 1024)]


class Line64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", w.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", w.DWORD), ("FileName", ctypes.c_char_p),
                ("Address", ctypes.c_ulonglong)]


class Symbols:
    SYMOPT_UNDNAME = 0x2
    SYMOPT_LOAD_LINES = 0x10

    def __init__(self, exe):
        self.dbghelp = ctypes.WinDLL("dbghelp.dll")
        self.process = w.HANDLE(0x1234)  # any value: an id, no real process is read
        self.exe = exe
        self.bases = {}
        self.next_base = 0x100000000
        d = self.dbghelp
        d.SymSetOptions(self.SYMOPT_UNDNAME | self.SYMOPT_LOAD_LINES)  # not deferred: a failed load shows at once
        d.SymInitialize.argtypes = [w.HANDLE, ctypes.c_char_p, w.BOOL]
        d.SymLoadModuleEx.restype = ctypes.c_ulonglong
        d.SymLoadModuleEx.argtypes = [w.HANDLE, w.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_ulonglong, w.DWORD, ctypes.c_void_p, w.DWORD]
        d.SymFromAddr.argtypes = [w.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SymbolInfo)]
        d.SymGetLineFromAddr64.argtypes = [w.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(w.DWORD), ctypes.POINTER(Line64)]
        d.SymInitialize(self.process, os.path.dirname(os.path.abspath(exe)).encode(), False)

    def base(self, module):
        key = module.lower()
        if key not in self.bases:
            if key == os.path.basename(self.exe).lower():
                path = self.exe
            else:
                path = os.path.join(os.environ["SystemRoot"], "System32", module)
            self.bases[key] = self.dbghelp.SymLoadModuleEx(self.process, None, path.encode(), None, self.next_base, 0, None, 0)
            self.next_base += 0x100000000
        return self.bases[key]

    def resolve(self, module, offset):
        base = self.base(module)
        if not base:
            return "%s+0x%x" % (module, offset)
        address = base + offset
        symbol = SymbolInfo()
        symbol.SizeOfStruct = 88  # sizeof(SYMBOL_INFO)
        symbol.MaxNameLen = 1024
        displacement = ctypes.c_ulonglong(0)
        if not self.dbghelp.SymFromAddr(self.process, address, ctypes.byref(displacement), ctypes.byref(symbol)):
            return "%s+0x%x" % (module, offset)
        text = "%s!%s+0x%x" % (module, symbol.Name.decode(errors="replace"), displacement.value)
        line = Line64()
        line.SizeOfStruct = ctypes.sizeof(Line64)
        column = w.DWORD(0)
        if self.dbghelp.SymGetLineFromAddr64(self.process, address, ctypes.byref(column), ctypes.byref(line)):
            text += "  " + os.path.basename(line.FileName.decode(errors="replace")) + ":" + str(line.LineNumber)
        return text


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    symbols = Symbols(sys.argv[1])
    for argument in sys.argv[2:]:
        lines = open(argument, encoding="utf-8", errors="replace").read().splitlines() if os.path.isfile(argument) else [argument]
        for line in lines:
            frames = FRAME.findall(line)
            if not frames:
                continue
            print(FRAME.split(line)[0].rstrip())
            for module, offset in frames:
                print("    " + symbols.resolve(module, int(offset, 16)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
