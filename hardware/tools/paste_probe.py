"""Try several ways of pasting into whatever window you click on.

Run it, then click into the target app within 4 seconds.  Each method puts
"probe N <how>" on the clipboard and tries to paste it.  Whatever lines appear
in the target tell you which mechanism that app accepts.

    uv run --project C:/Users/khmal/Projects/fethr python hardware/tools/paste_probe.py
"""
import ctypes
import sys
import time
from ctypes import wintypes

import pyperclip

u = ctypes.windll.user32
KEYEVENTF_KEYUP, KEYEVENTF_SCANCODE = 0x0002, 0x0008
VK_CONTROL, VK_V, VK_LCONTROL = 0x11, 0x56, 0xA2
SC_LCTRL, SC_V = 0x1D, 0x2F
WM_PASTE = 0x0302


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong))]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("pad", ctypes.c_byte * 32)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def send(events):
    arr = (INPUT * len(events))()
    for i, (vk, sc, flags) in enumerate(events):
        arr[i].type = 1
        arr[i].ki = KEYBDINPUT(vk, sc, flags, 0, None)
    return u.SendInput(len(events), arr, ctypes.sizeof(INPUT))


def fg():
    h = u.GetForegroundWindow()
    t = ctypes.create_unicode_buffer(256)
    u.GetWindowTextW(h, t, 256)
    return h, t.value


def focused_control():
    class GUITHREADINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("flags", wintypes.DWORD), ("hwndActive", wintypes.HWND),
                    ("hwndFocus", wintypes.HWND), ("hwndCapture", wintypes.HWND), ("hwndMenuOwner", wintypes.HWND),
                    ("hwndMoveSize", wintypes.HWND), ("hwndCaret", wintypes.HWND), ("rcCaret", wintypes.RECT)]
    h, _ = fg()
    tid = u.GetWindowThreadProcessId(h, None)
    gti = GUITHREADINFO(); gti.cbSize = ctypes.sizeof(GUITHREADINFO)
    u.GetGUIThreadInfo(tid, ctypes.byref(gti))
    return gti.hwndFocus


METHODS = [
    ("vk-only SendInput", lambda: send([(VK_CONTROL, 0, 0), (VK_V, 0, 0), (VK_V, 0, KEYEVENTF_KEYUP), (VK_CONTROL, 0, KEYEVENTF_KEYUP)])),
    ("scancode SendInput", lambda: send([(0, SC_LCTRL, KEYEVENTF_SCANCODE), (0, SC_V, KEYEVENTF_SCANCODE),
                                         (0, SC_V, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP), (0, SC_LCTRL, KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP)])),
    ("vk+scancode SendInput", lambda: send([(VK_LCONTROL, SC_LCTRL, 0), (VK_V, SC_V, 0), (VK_V, SC_V, KEYEVENTF_KEYUP), (VK_LCONTROL, SC_LCTRL, KEYEVENTF_KEYUP)])),
    ("WM_PASTE to focused control", lambda: u.SendMessageW(focused_control(), WM_PASTE, 0, 0)),
]
try:
    import keyboard
    METHODS.insert(0, ("keyboard.send", lambda: keyboard.send("ctrl+v")))
except Exception:
    pass

print("click into the target app now...")
for i in range(4, 0, -1):
    print(i); time.sleep(1)
h, title = fg()
print(f"target: {title!r} hwnd={h} focused_control={focused_control()}")
for n, (name, fn) in enumerate(METHODS, 1):
    line = f"probe {n} {name}\r\n"
    pyperclip.copy(line)
    time.sleep(0.1)
    try:
        r = fn()
    except Exception as exc:
        r = f"EXC {exc}"
    print(f"{n}. {name}: sent -> {r}")
    time.sleep(1.0)
print("done - which probe lines appeared in the target?")
