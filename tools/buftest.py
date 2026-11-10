"""Шаг 1 разбора: wblk -> буфер ОЗУ; читаем медленно (rblk, проверенный) и быстро (rblkf). Сравниваем."""
import sys, struct
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txwprog as T

pr = T.Prog(T.PORT_DEFAULT)
print("HID", hex(pr.hid()), "debug", pr.debug())
BUF = 0x200011c8
pat = [(0x11111111 * ((i % 15) + 1)) ^ (i << 8) for i in range(64)]
pr.wblk(BUF, pat)
slow = pr.rblk(BUF, 64)
fast = pr.rblkf(BUF, 64)
print("шаблон  :", " ".join(f"{w:08x}" for w in pat[:6]))
print("rblk    :", " ".join(f"{w:08x}" for w in slow[:6]))
print("rblkf   :", " ".join(f"{w:08x}" for w in fast[:6]))
print("rblk==шаблон:", slow == pat, " rblkf==шаблон:", fast == pat)
diff = [i for i in range(64) if fast[i] != pat[i]]
print("rblkf расходится в словах:", diff[:20])
if diff:
    i = diff[0]
    print(f"  слово {i}: ждали {pat[i]:08x} получили {fast[i]:08x}  xor {pat[i] ^ fast[i]:08x}")
