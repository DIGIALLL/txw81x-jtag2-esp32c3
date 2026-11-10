// jtag2.h — двухпроводной отладочный протокол C-SKY (JTAG2 / CKHAD) ногодрыгом.
// Восстановлен по прошивке CK-Link Lite 2.36 (см. re/NOTES.md).
//
// Кадр: бит выставляется на HDA, затем такт HCK 0->1. Все поля младшим битом вперёд.
//   IR write : 0 0 0 1 1 | IR[n]  | par | 1 1 | +1 такт | idle
//   DR write : 0 0 1 1 1 | DR[n]  | par | 1 1 | +1 такт | idle
//   DR read  : 0 1 1 1 1 | TRN(вход) | DR[n] | par TRN | выход, HDA=1, 2 такта | idle
//   par = NOT(XOR бит данных)
// IR = HACR: bit7 R/W(1=чтение), bit6 GO, bit5 EX, bits4..0 RS (номер регистра HAD).
#pragma once
#include <Arduino.h>
#include "soc/gpio_reg.h"
#include "driver/gpio.h"

namespace jtag2 {

static int PIN_HCK = 5;
static int PIN_HDA = 6;
static int PIN_RST = 7;
static uint32_t halfUs = 2;      // полупериод такта, мкс (2 мкс ≈ 250 кГц)
static uint8_t idleClk = 8;      // тактов покоя после кадра (HDA=1)
static uint8_t irBits = 8;       // длина HACR
static bool padTo32 = true;      // DR дополняется до 32 бит (флаг CK-Link == 0)
static int lastParityErr = 0;

static uint32_t nopLoops = 0;    // доп. задержка в циклах при halfUs=0
static inline void dly() { if (halfUs) delayMicroseconds(halfUs); else for (volatile uint32_t i = 0; i < nopLoops; i++) {} }
// Быстрый доступ к GPIO через регистры
static inline void hda(int v) { REG_WRITE(v ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1u << PIN_HDA); }
static inline void clk() {        // HCK 0 -> 1
  REG_WRITE(GPIO_OUT_W1TC_REG, 1u << PIN_HCK); dly();
  REG_WRITE(GPIO_OUT_W1TS_REG, 1u << PIN_HCK); dly();
}
static inline int hdaIn() { return (REG_READ(GPIO_IN_REG) >> PIN_HDA) & 1; }
static inline void bitOut(int v) { hda(v); clk(); }
static inline void dirIn()  { REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << PIN_HDA); }
static inline void dirOut() { hda(1); REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << PIN_HDA); }

static void begin() {
  pinMode(PIN_HCK, OUTPUT); digitalWrite(PIN_HCK, HIGH);
  pinMode(PIN_HDA, OUTPUT); digitalWrite(PIN_HDA, HIGH);
  gpio_pullup_en((gpio_num_t)PIN_HDA);
  dirOut();
  pinMode(PIN_RST, INPUT);       // RST не трогаем (Hi-Z)
}

static void release() {          // отпустить линии (Hi-Z)
  pinMode(PIN_HCK, INPUT); pinMode(PIN_HDA, INPUT); pinMode(PIN_RST, INPUT);
}

static void idle(uint8_t n) { hda(1); for (uint8_t i = 0; i < n; i++) clk(); }

// Сброс/синхронизация линии: много тактов с HDA=1.
static void lineReset(uint16_t n = 80) { dirOut(); idle(n > 255 ? 255 : n); if (n > 255) idle(n - 255); }

static void header(int b1, int b2) { bitOut(0); bitOut(b1); bitOut(b2); bitOut(1); bitOut(1); }

static void trailer() { bitOut(1); bitOut(1); clk(); idle(idleClk); }

// Запись IR (HACR)
static void irWrite(uint32_t ir) {
  int x = 0;
  header(0, 0);
  for (uint8_t i = 0; i < irBits; i++) { int b = (ir >> i) & 1; x ^= b; bitOut(b); }
  bitOut(!x);
  trailer();
}

// Запись DR
static void drWrite(uint32_t v, uint8_t nbits = 32) {
  int x = 0;
  header(0, 1);
  uint8_t total = (padTo32 && nbits < 32) ? 32 : nbits;
  for (uint8_t i = 0; i < total; i++) { int b = (i < nbits && i < 32) ? (v >> i) & 1 : 0; x ^= b; bitOut(b); }
  bitOut(!x);
  trailer();
}

// Чтение DR. Возвращает значение; parity-ошибка -> lastParityErr
static uint32_t drRead(uint8_t nbits = 32) {
  uint32_t v = 0; int x = 0;
  header(1, 1);
  dirIn();
  clk();                                     // TRN
  uint8_t total = (padTo32 && nbits < 32) ? 32 : nbits;
  for (uint8_t i = 0; i < total; i++) {
    clk();
    int b = hdaIn();
    if (i < 32) v |= (uint32_t)b << i;
    x ^= b;
  }
  clk();                                     // parity
  int p = hdaIn();
  lastParityErr = (p == x) ? 1 : 0;          // ожидаем p = NOT(x)
  clk();                                     // TRN
  dirOut();
  hda(1); clk(); clk();
  idle(idleClk);
  return nbits < 32 ? (v & ((1u << nbits) - 1)) : v;
}

// Регистры HAD
static uint32_t hadRead(uint8_t rs, uint8_t nbits = 32) { irWrite(0x80 | (rs & 0x1f)); return drRead(nbits); }
static void hadWrite(uint8_t rs, uint32_t v, uint8_t nbits = 32) { irWrite(rs & 0x1f); drWrite(v, nbits); }

enum : uint8_t { HID = 0x02, HCR = 0x0d, HSR = 0x0e, EHSR = 0x0f, WBBR = 0x11, PSR = 0x12, PC = 0x13,
                 CSR = 0x15, DDCADDR = 0x18, DDCDATA = 0x19, CPUSEL = 0x1a, CPUST = 0x1b, HCDI = 0x1f };

// Память через DDC
static uint32_t memRead32(uint32_t addr) { hadWrite(DDCADDR, addr); return hadRead(DDCDATA); }


// ---- Выполнение инструкций в режиме отладки (CKHAD, ABIv2/CK80x), по CK-Link 0x800afc0 ----
// HACR: 0x95 чтение CSR, 0x15 запись CSR, 0x11/0x91 WBBR, 0x54 = GO|IR (выполнить инструкцию)
static inline uint16_t insMov(uint8_t r) { return (uint16_t)(((0x6c | (r >> 2)) << 8) | ((r << 6) | (r << 2) | 3)) ; }

static uint32_t lastHsr = 0;
static uint8_t settleUs = 50;
static void waitDone() {             // дать инструкции завершиться: пауза + чтение HSR
  delayMicroseconds(settleUs);
  for (int i = 0; i < 8; i++) { lastHsr = hadRead(HSR); if ((lastHsr & 3) == 2) break; }
}

static bool enterDebug() {
  // Уже в отладке (например, стоим на bkpt) — НЕ взводить DR: запрос «запоминается»
  // и срывает следующий запуск (CPU останавливается, не выполнив код).
  if ((hadRead(HSR) & 3) == 2) return true;
  hadWrite(HCR, 0x8000);
  bool ok = false;
  for (int i = 0; i < 20; i++) { delay(10); if ((hadRead(HSR) & 3) == 2) { ok = true; break; } }
  return ok;
}

// Выполнить 16-битную инструкцию; ffy=1 — операнд из WBBR (для записи регистра)
static void execInsn(uint16_t insn, bool wr, uint32_t wbbr, uint32_t* out) {
  uint16_t csr = hadRead(CSR) & 0xfe7f;   // биты 7,8 — от запуска, убрать
  uint16_t c2 = csr | 0x0080;
  uint8_t hi = ((c2 >> 8) & 0xfe) | 6 | (wr ? 1 : 0);
  c2 = (c2 & 0x00ff) | (hi << 8);
  hadWrite(CSR, c2);
  if (wr) hadWrite(WBBR, wbbr);
  irWrite(0x54); drWrite(((uint32_t)insn << 16) | c2);
  waitDone();
  if (out) *out = hadRead(WBBR);
  hadWrite(CSR, csr);
}

static void regWrite(uint8_t r, uint32_t v) { execInsn(insMov(r), true, v, nullptr); }
static uint32_t regRead(uint8_t r) { uint32_t v = 0; execInsn(insMov(r), false, 0, &v); return v; }

// Простое выполнение без WBBR (ld/st): IR с GO, CSR без изменений
static void execPlain(uint16_t insn) {
  uint16_t csr = hadRead(CSR) & 0xfe7f;
  irWrite(0x54); drWrite(((uint32_t)insn << 16) | csr);
  waitDone();
}

static uint32_t readRetries = 0;
static bool regWriteV(uint8_t r, uint32_t v) {      // запись с проверкой
  for (int i = 0; i < 6; i++) { regWrite(r, v); if (regRead(r) == v) return true; readRetries++; }
  return false;
}
static uint32_t cpuRead32(uint32_t addr) {
  uint32_t prev = 0; bool have = false;
  for (int i = 0; i < 8; i++) {
    regWriteV(1, addr);
    execPlain(0x9140);                               // ld16.w r2,(r1,0)
    uint32_t v = regRead(2);
    if (have && v == prev && regRead(1) == addr) return v;
    prev = v; have = true; readRetries++;
  }
  return prev;
}
static bool cpuWrite32(uint32_t addr, uint32_t v) {
  for (int i = 0; i < 6; i++) {
    regWriteV(1, addr); regWriteV(2, v); execPlain(0xb140);   // st16.w r2,(r1,0)
    if (cpuRead32(addr) == v) return true;
  }
  return false;
}


// Запуск CPU с адреса pc и ожидание повторного входа в отладку (bkpt). Возвращает время в мс или -1.
static uint32_t hcrRun = 0x4000;   // HCR при запуске (без DR=0x8000, иначе пошаговый режим)
static uint16_t csrRunOr = 0x0080; // CSR.bit7: bkpt -> вход в отладку (найдено перебором)
static int runTo(uint32_t pc, uint32_t timeoutMs) {
  hadWrite(PC, pc);
  hadWrite(HCR, hcrRun);
  uint16_t csr = (hadRead(CSR) & 0xfe7f) | csrRunOr;
  hadWrite(CSR, csr);
  irWrite(0x74); drWrite((0x6c03u << 16) | csr);   // GO|EX|IR, nop -> выход из отладки
  uint32_t t0 = millis();
  delay(1);
  while (millis() - t0 < timeoutMs) {
    if ((hadRead(HSR) & 3) == 2) { int t = millis() - t0; return t; }
    delay(1);
  }
  return -1;
}


// ---- Блочные операции ----
// addi16 r1, 4 = 0x2103 ; ld16.w r2,(r1,0) = 0x9140 ; st16.w r2,(r1,0) = 0xb140
static void blkWrite(uint32_t addr, const uint32_t* w, uint32_t n) {
  regWriteV(1, addr);
  for (uint32_t i = 0; i < n; i++) {
    regWriteV(2, w[i]);
    execPlain(0xb140);
    execPlain(0x2103);
    if (regRead(1) != addr + 4 * (i + 1)) { regWriteV(1, addr + 4 * (i + 1)); }
  }
}
static void blkRead(uint32_t addr, uint32_t* w, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) w[i] = cpuRead32(addr + 4 * i);
}
// Вызов функции: r0..r2 аргументы, r14 = стек, r15 = адрес bkpt. Возвращает r0 или 0xDEAD0000|err.
static uint32_t callFn(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t sp, uint32_t lr, uint32_t to, int* ms) {
  regWriteV(0, a0); regWriteV(1, a1); regWriteV(2, a2); regWriteV(14, sp); regWriteV(15, lr);
  *ms = runTo(fn, to);
  if (*ms < 0) { hadWrite(HCR, 0x8000); delay(20); return 0xDEAD0001; }
  return regRead(0);
}


// Быстрое чтение блока: r1 = адрес, затем на слово: ld r2,(r1) ; mov r2 -> WBBR ; addi r1,4
static void blkReadFast(uint32_t addr, uint32_t* w, uint32_t n) {
  regWriteV(1, addr);
  uint16_t csr = hadRead(CSR) & 0xfe7f;
  uint16_t cr = csr | 0x0080; cr = (cr & 0x00ff) | ((((cr >> 8) & 0xfe) | 6) << 8);   // режим чтения регистра
  for (uint32_t i = 0; i < n; i++) {
    hadWrite(CSR, csr); irWrite(0x54); drWrite((0x9140u << 16) | csr); hadRead(HSR);        // ld16.w r2,(r1)
    hadWrite(CSR, cr);  irWrite(0x54); drWrite(((uint32_t)insMov(2) << 16) | cr); hadRead(HSR);
    w[i] = hadRead(WBBR);
    hadWrite(CSR, csr); irWrite(0x54); drWrite((0x2103u << 16) | csr); hadRead(HSR);        // addi r1,4
  }
}

// Быстрая запись блока: r1 = адрес; на слово: WBBR=v ; mov r2<-WBBR ; st r2,(r1) ; addi r1,4
static void blkWriteFast(uint32_t addr, const uint32_t* w, uint32_t n) {
  regWriteV(1, addr);
  uint16_t csr = hadRead(CSR) & 0xfe7f;
  uint16_t cw = csr | 0x0080; cw = (cw & 0x00ff) | ((((cw >> 8) & 0xfe) | 6 | 1) << 8);   // режим записи регистра
  for (uint32_t i = 0; i < n; i++) {
    hadWrite(CSR, cw);  hadWrite(WBBR, w[i]); irWrite(0x54); drWrite(((uint32_t)insMov(2) << 16) | cw); hadRead(HSR);
    hadWrite(CSR, csr); irWrite(0x54); drWrite((0xb140u << 16) | csr); hadRead(HSR);        // st16.w r2,(r1)
    irWrite(0x54); drWrite((0x2103u << 16) | csr); hadRead(HSR);                            // addi r1,4
  }
  hadWrite(CSR, csr);
}
}  // namespace jtag2
