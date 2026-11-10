#!/usr/bin/env python3
"""txwprog — дамп/прошивка SPI-флеша TXW81x через самодельный JTAG2-программатор на ESP32-C3.

Программатор: прошивка fw/txw-prog (HCK=GPIO7, HDA=GPIO5 на текущей проводке).
Алгоритм флеша: Taixin_TXW81X_FLASH_ALGORITHM.elf (из openshwprojects/FlashTools),
грузится в ОЗУ камеры с 0x20000000 и вызывается функциями (возврат на bkpt).

Примеры:
  python txwprog.py --port COM11 id
  python txwprog.py --port COM11 dump backup.bin --size 0x100000
"""
import argparse, os, struct, sys, time
import serial
from elftools.elf.elffile import ELFFile

# Алгоритм флеша не входит в репозиторий: скачать Taixin_TXW81X_FLASH_ALGORITHM.elf
# из github.com/openshwprojects/FlashTools и положить как re/flashalgo/txw81x_algo.elf
ALGO_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "re", "flashalgo", "txw81x_algo.elf")
PORT_DEFAULT = os.environ.get("TXW_PORT", "COM11")
BKPT_ADDR = 0x20003000
SCRATCH = 0x20003010


class Prog:
    def __init__(self, port, hck=7, hda=5, verbose=False):
        self.p = serial.Serial(port, 115200, timeout=0.05)
        self.p.dtr = False
        self.p.rts = False
        self.v = verbose
        time.sleep(0.5)
        self.p.read(1 << 20)
        self.cmd("!log 0", "[j] log")
        self.cmd(f"!jpins {hck} {hda} 3", "[j] HCK")
        self.cmd("!jinit", "[j] init")
        self.cmd("!jspeed 0", "[j] halfUs")
        self.cmd("!jnop 0", "[j] nop")

    def cmd(self, c, expect="[j]", timeout=30.0):
        self.p.write((c + "\n").encode())
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            buf += self.p.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("utf-8", "replace").strip()
                if s.startswith(expect):
                    if self.v:
                        print("  <", s[:160])
                    return s
                if self.v or True:
                    self.last_other = s
        raise TimeoutError(f"{c[:30]} | хвост: {buf[:60]!r}…{buf[-40:]!r} len={len(buf)} | прочее: {getattr(self, 'last_other', '')[:60]!r}")

    def hid(self):
        return int(self.cmd("!hid", "[j] HID").split("HID=")[1].split()[0], 16)

    def debug(self):
        s = self.cmd("!dbg", "[j] debug")
        return "debug=1" in s

    def wblk(self, addr, words):
        for i in range(0, len(words), 16):
            chunk = words[i:i + 16]
            h = "".join(f"{w:08x}" for w in chunk)
            for attempt in range(4):
                s = self.cmd(f"!wblk 0x{addr + 4 * i:08x} {h}", "[j] wblk")
                if "bad=0" in s:
                    break
            else:
                raise IOError(f"запись не сошлась @0x{addr + 4 * i:08x}: {s}")

    def wblkf(self, addr, words):
        """Быстрая запись в ОЗУ камеры: по 64 слова, XOR-маска + сумма, программатор сам перечитывает."""
        for i in range(0, len(words), 64):
            chunk = words[i:i + 64]
            h = "".join(f"{(w ^ (0x9E3779B9 * (k + 1))) & 0xffffffff:08x}" for k, w in enumerate(chunk))
            sm = sum(chunk) & 0xffffffff
            for attempt in range(6):
                try:
                    s = self.cmd(f"!wblkf 0x{addr + 4 * i:08x} {h} s{sm:08x}", "[j] wblkf", timeout=6)
                except TimeoutError:
                    self.p.write(b"\n"); time.sleep(0.2); self.p.read(1 << 20)
                    continue
                if " bad=0" in s:
                    break
            else:
                raise IOError(f"wblkf не сошлась @0x{addr + 4 * i:08x}")

    def rblk(self, addr, nwords):
        out = []
        for i in range(0, nwords, 256):
            n = min(256, nwords - i)
            s = self.cmd(f"!rblk 0x{addr + 4 * i:08x} {n}", "[j] rblk")
            h = s.split()[-1]
            out += [int(h[k:k + 8], 16) for k in range(0, len(h), 8)]
        return out

    def rblkf(self, addr, nwords):
        out = []
        for i in range(0, nwords, 64):
            n = min(64, nwords - i)
            for _ in range(6):
                try:
                    s = self.cmd(f"!rblkf 0x{addr + 4 * i:08x} {n}", "[j] rblkf", timeout=4)
                except TimeoutError:
                    self.timeouts = getattr(self, "timeouts", 0) + 1
                    self.p.write(b"\n"); time.sleep(0.2); self.p.read(1 << 20)
                    continue
                if " ERR " in s:
                    continue
                parts = s.split()
                h, sm = parts[-2], parts[-1]
                if len(h) != 8 * n or not sm.startswith("s"):
                    continue
                ws = [int(h[k:k + 8], 16) ^ ((0x9E3779B9 * (k // 8 + 1)) & 0xffffffff) for k in range(0, len(h), 8)]
                if (sum(ws) & 0xffffffff) != int(sm[1:], 16):
                    continue
                out += ws
                break
            else:
                raise IOError(f"rblkf 0x{addr + 4 * i:08x} не читается")
        return out

    def rblk_checked(self, addr, nwords):
        a = self.rblkf(addr, nwords)
        for _ in range(4):
            b = self.rblkf(addr, nwords)
            if a == b:
                return a, True
            a = b
        return a, False

    def call(self, fn, a0=0, a1=0, a2=0, timeout=3000):
        s = self.cmd(f"!call 0x{fn:08x} 0x{a0:x} 0x{a1:x} 0x{a2:x} {timeout}", "[j] call", timeout=timeout / 1000 + 10)
        r0 = int(s.split("r0=")[1].split()[0], 16)
        if self.v:
            print(" ", s)
        return r0, s


def load_algo(pr, path):
    f = ELFFile(open(path, "rb"))
    syms = {s.name: s["st_value"] for s in f.get_section_by_name(".symtab").iter_symbols() if s.name}
    for seg in f.iter_segments():
        if seg["p_type"] != "PT_LOAD":
            continue
        data = seg.data() + b"\0" * (seg["p_memsz"] - seg["p_filesz"])
        data += b"\0" * (-len(data) % 4)
        words = list(struct.unpack(f"<{len(data) // 4}I", data))
        base = seg["p_vaddr"]
        # g_rwBuffer (4 КБ) можно не обнулять — экономим время
        rb, rbn = syms.get("g_rwBuffer"), 4096
        t0 = time.time()
        if rb and base <= rb < base + len(data):
            k0, k1 = (rb - base) // 4, (rb - base + rbn) // 4
            pr.wblk(base, words[:k0])
            pr.wblk(rb + rbn, words[k1:])
        else:
            pr.wblk(base, words)
        print(f"алгоритм загружен: 0x{base:08x}..0x{base + len(data):08x} за {time.time() - t0:.1f} с")
    pr.wblk(BKPT_ADDR, [0x00000000])  # bkpt16 bkpt16
    for _ in range(2):                # сбросить «запомненный» запрос отладки, если он есть
        pr.call(BKPT_ADDR, timeout=500)
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=PORT_DEFAULT, help="COM-порт программатора (или переменная TXW_PORT)")
    ap.add_argument("--algo", default=ALGO_DEFAULT)
    ap.add_argument("-v", action="store_true")
    sub = ap.add_subparsers(dest="op", required=True)
    sub.add_parser("id")
    d = sub.add_parser("dump")
    d.add_argument("out")
    d.add_argument("--addr", type=lambda x: int(x, 0), default=0)
    d.add_argument("--size", type=lambda x: int(x, 0), default=0x100000)
    d.add_argument("--resume", action="store_true")
    w = sub.add_parser("write")
    w.add_argument("image")
    w.add_argument("--addr", type=lambda x: int(x, 0), default=0)
    w.add_argument("--no-chip-erase", action="store_true")
    w.add_argument("--skip-erase", action="store_true", help="не стирать (продолжение); пустота проверяется поблочно")
    w.add_argument("--max-blocks", type=int, default=0, help="записать не больше N непустых блоков")
    w.add_argument("--start", type=lambda x: int(x, 0), default=0, help="смещение в образе, с которого начать")
    w.add_argument("--precheck", action="store_true", help="проверять пустоту каждого блока перед записью")
    w.add_argument("--yes", action="store_true", help="без этого флага запись не выполняется")
    a = ap.parse_args()
    if a.op == "write" and not a.yes:
        sys.exit("запись стирает флеш камеры; добавьте --yes")

    pr = Prog(a.port, verbose=a.v)
    print(f"HID=0x{pr.hid():08x}")
    if not pr.debug():
        sys.exit("не удалось остановить CPU (режим отладки)")
    syms = load_algo(pr, a.algo)
    r, s = pr.call(syms["flashInit"], timeout=3000)
    print(f"flashInit -> 0x{r:08x}  ({s})")
    pr.wblk(SCRATCH, [0])
    r, s = pr.call(syms["flashID"], SCRATCH)
    fid = pr.rblk(SCRATCH, 1)[0]
    print(f"flashID -> r0=0x{r:08x}, ID=0x{fid:08x}")
    if a.op == "id":
        return

    buf = syms["g_rwBuffer"]
    CH = 4096
    if a.op == "write":
        img = open(a.image, "rb").read()
        img += b"\xff" * (-len(img) % CH)
        # Стирание блоками по 64 КБ (flashErase ждёт готовности; chipErase возвращался за 6 мс без стирания)
        erase_end = 0x100000 if not a.no_chip_erase else a.addr + len(img)

        def wait_busy():
            pr.call(syms["spiflash_wait_busy"], timeout=20000)

        def is_blank(off):
            pr.call(syms["flashRead"], buf, off, CH)
            ws, ok = pr.rblk_checked(buf, CH // 4)
            return ok and all(x == 0xffffffff for x in ws)

        # flashErase только ПОДАЁТ команду (возврат за 2-3 мс) — ждём готовности и проверяем
        for eoff in (range(a.addr & ~0xffff, erase_end, 0x10000) if not a.skip_erase else []):
            for attempt in range(4):
                r, s = pr.call(syms["flashErase"], eoff, 0x10000, 0, timeout=20000)
                if r != 0:
                    sys.exit(f"\nстирание вернуло ошибку @0x{eoff:06x}: {s}")
                wait_busy(); time.sleep(0.4); wait_busy()
                if is_blank(eoff) and is_blank(eoff + 0x10000 - CH):
                    break
                print(f"\n  ! блок 0x{eoff:06x} не стёрся, повтор")
            else:
                sys.exit(f"блок 0x{eoff:06x} не стирается — запись не начинаю")
            print(f"\rстирание 0x{eoff:06x} ok", end="")
        print()

        def read_flash(off):
            for attempt in range(5):
                try:
                    pr.call(syms["flashRead"], buf, off, CH)
                    ws, ok = pr.rblk_checked(buf, CH // 4)
                    if ok:
                        return struct.pack(f"<{CH // 4}I", *ws)
                except (TimeoutError, IOError):
                    pr.p.write(b"\n"); time.sleep(0.5); pr.p.read(1 << 20)
                    pr.cmd("!jinit", "[j] init")
            raise IOError(f"не читается 0x{off:06x}")

        # Пошагово: пусто? -> буфер -> сверка буфера -> запись -> ожидание -> чтение назад -> сравнение
        t0 = time.time()
        nblk = len(img) // CH
        written = 0
        for bi, off in enumerate(range(0, len(img), CH)):
            chunk = img[off:off + CH]
            fa = a.addr + off
            if off < a.start or chunk == b"\xff" * CH:
                continue
            if a.max_blocks and written >= a.max_blocks:
                print(f"\nостановлено по --max-blocks; продолжить: --skip-erase --start 0x{off:x}")
                return
            written += 1
            if a.precheck:
                pre = read_flash(fa)
                if pre != b"\xff" * CH:
                    sys.exit(f"\nблок 0x{fa:06x}: перед записью НЕ пуст ({CH - pre.count(0xff)} байт не FF) — стоп")
            words = list(struct.unpack(f"<{CH // 4}I", chunk))
            for attempt in range(4):
                pr.wblkf(buf, words)                 # программатор сам сверил буфер
                if True:
                    break
                print(f"\n  ! 0x{fa:06x}: буфер в ОЗУ не совпал, повтор")
            else:
                sys.exit(f"\nблок 0x{fa:06x}: не удаётся заполнить буфер — стоп")
            r, s = pr.call(syms["flashProgram"], fa, buf, CH, timeout=5000)
            if r != 0 or "PC=0x20003000" not in s:
                sys.exit(f"\nблок 0x{fa:06x}: flashProgram вернулась нештатно: {s} — стоп")
            wait_busy()
            pr.call(syms["flashRead"], buf, fa, CH)
            post = struct.pack(f"<{CH // 4}I", *pr.rblkf(buf, CH // 4))
            if post != chunk:
                post = read_flash(fa)                 # перепроверка двойным чтением
            if post != chunk:
                nd = sum(x != y for x, y in zip(post, chunk))
                sys.exit(f"\nблок 0x{fa:06x}: после записи {nd} байт не совпали — стоп")
            el = time.time() - t0
            print(f"\rблок {bi + 1}/{nblk} 0x{fa:06x} ok  {(off + CH) / el / 1024:4.2f} КБ/с  осталось ~{(len(img) - off - CH) / ((off + CH) / el) / 60:4.1f} мин", end="")
        print(f"\nготово: все {nblk} блоков записаны и проверены по одному")
        return
        print("\nсверка...")
        bad = 0
        for off in range(0, len(img), CH):
            for attempt in range(5):
                try:
                    r, s = pr.call(syms["flashRead"], buf, a.addr + off, CH)
                    words, ok = pr.rblk_checked(buf, CH // 4)
                    if ok:
                        break
                except (TimeoutError, IOError):
                    pr.p.write(b"\n"); time.sleep(0.5); pr.p.read(1 << 20)
                    pr.cmd("!jinit", "[j] init")
            got = struct.pack(f"<{CH // 4}I", *words)
            if got != img[off:off + CH]:
                bad += 1
                print(f"\n  ! расхождение в блоке 0x{off:06x}")
            print(f"\rсверка 0x{off:06x}  {(off + CH) * 100 // len(img):3d}%", end="")
        print(f"\nготово: записано {len(img)} байт, блоков с расхождением: {bad}")
        return
    t0 = time.time()
    import os
    start = a.addr
    mode = "wb"
    if a.resume and os.path.exists(a.out):
        have = os.path.getsize(a.out) // CH * CH
        start = a.addr + have
        mode = "r+b"
        print(f"продолжаю с 0x{start:06x}")
    with open(a.out, mode) as fo:
        fo.seek(start - a.addr)
        fo.truncate()
        for off in range(start, a.addr + a.size, CH):
            n = min(CH, a.addr + a.size - off)
            for attempt in range(5):
                try:
                    r, s = pr.call(syms["flashRead"], buf, off, n)
                    words, ok = pr.rblk_checked(buf, (n + 3) // 4)
                    if ok:
                        break
                    print(f"\n  ! блок 0x{off:06x}: чтения не совпали, повтор")
                except (TimeoutError, IOError) as e:
                    print(f"\n  ! блок 0x{off:06x}: {e!s:.40}, переинициализация")
                    pr.p.write(b"\n"); time.sleep(0.5); pr.p.read(1 << 20)
                    pr.cmd("!jinit", "[j] init")
            else:
                raise IOError(f"блок 0x{off:06x} не читается")
            fo.write(struct.pack(f"<{len(words)}I", *words)[:n])
            fo.flush()
            done = off + n - start
            el = time.time() - t0
            print(f"\r0x{off:06x}  {(off + n - a.addr) * 100 // a.size:3d}%  {done / el / 1024:5.1f} КБ/с  осталось ~{(a.addr + a.size - off - n) / (done / el) / 60:4.1f} мин", end="")
    print(f"\nготово: {a.out}")


if __name__ == "__main__":
    main()
