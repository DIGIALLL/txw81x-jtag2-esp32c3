"""Стереть один 64К-блок с ожиданием и проверкой всех 16 секторов. Аргумент: адрес блока."""
import sys, struct, time
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import txwprog as T

blk = int(sys.argv[1], 0)
pr = T.Prog(T.PORT_DEFAULT)
print("HID", hex(pr.hid()), "debug", pr.debug())
syms = T.load_algo(pr, T.ALGO_DEFAULT)
print("flashInit", hex(pr.call(syms["flashInit"], timeout=3000)[0]))
buf = syms["g_rwBuffer"]
r, s = pr.call(syms["flashErase"], blk, 0x10000, 0, timeout=20000)
print("erase:", s)
pr.call(syms["spiflash_wait_busy"], timeout=20000); time.sleep(0.5); pr.call(syms["spiflash_wait_busy"], timeout=20000)
bad = []
for off in range(blk, blk + 0x10000, 0x1000):
    pr.call(syms["flashRead"], buf, off, 0x1000)
    ws = pr.rblkf(buf, 1024)
    if any(w != 0xffffffff for w in ws):
        bad.append(hex(off))
print("не стёрто:", bad if bad else "всё FF")
