"""Диагностика: какие 4К-сектора первого 64К-блока не стёрты; повторное стирание."""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txwprog as T

pr = T.Prog(T.PORT_DEFAULT)
print("HID", hex(pr.hid()), "debug", pr.debug())
syms = T.load_algo(pr, T.ALGO_DEFAULT)
print("flashInit", hex(pr.call(syms["flashInit"], timeout=3000)[0]))
buf = syms["g_rwBuffer"]


def state():
    out = []
    for off in range(0, 0x10000, 0x1000):
        r, s = pr.call(syms["flashRead"], buf, off, 0x1000); print("   ", s)
        ws, ok = pr.rblk_checked(buf, 1024)
        nff = sum(1 for w in ws if w != 0xffffffff)
        out.append("." if nff == 0 else "X")
    return "".join(out)


print("до:     ", state()); print("тайм-аутов:", getattr(pr, "timeouts", 0))
for k in range(2):
    r, s = pr.call(syms["flashErase"], 0, 0x10000, 0, timeout=20000)
    print("erase ->", hex(r), s)
    print(f"после {k + 1}:", state())
