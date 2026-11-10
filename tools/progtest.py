"""Диагностика записи: пишем тестовый 4К-сектор в конец флеша разными способами и читаем обратно."""
import sys, struct, time
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txwprog as T

pr = T.Prog(T.PORT_DEFAULT)
print("HID", hex(pr.hid()), "debug", pr.debug())
syms = T.load_algo(pr, T.ALGO_DEFAULT)
print("flashInit", hex(pr.call(syms["flashInit"], timeout=3000)[0]))
buf = syms["g_rwBuffer"]
CH = 4096


def rd(off):
    pr.call(syms["flashRead"], buf, off, CH)
    ws, ok = pr.rblk_checked(buf, CH // 4)
    return struct.pack(f"<{CH // 4}I", *ws)


pat = bytes((i * 7 + 3) & 0xff for i in range(CH))
words = list(struct.unpack(f"<{CH // 4}I", pat))

for name, off, use_wait in (("без wait_busy", 0xFE000, False), ("с wait_busy", 0xFF000, True)):
    before = rd(off)
    print(f"[{name}] до: FF={before.count(0xff)}/{CH}")
    pr.wblk(buf, words)
    chk = struct.pack(f"<{CH // 4}I", *pr.rblkf(buf, CH // 4))
    print(f"   буфер в ОЗУ совпадает с шаблоном: {chk == pat}")
    r, s = pr.call(syms["flashProgram"], off, buf, CH, timeout=5000)
    print("  ", s)
    if use_wait:
        print("   wait:", pr.call(syms["spiflash_wait_busy"], timeout=5000)[1])
    time.sleep(0.3)
    after = rd(off)
    print(f"   после: совпало {sum(x == y for x, y in zip(after, pat))}/{CH}, FF={after.count(0xff)}")
