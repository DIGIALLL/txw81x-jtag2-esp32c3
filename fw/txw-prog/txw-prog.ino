// txw-prog — ESP32-C3 SuperMini
// Этап 1: мост UART -> USB для чтения лога загрузки TXW817 (пад DP).
// Этап 2 (позже): программатор JTAG2/CKHAD на GPIO4 (HCK) / GPIO5 (HDA).
//
// Проводка:
//   C3 GPIO20 / GPIO21 <- кандидаты в TX камеры (обе ноги — только входы)
//   C3 GND        -- GND камеры
//
// Команды с ПК (строка, начинается с '!'):
//   !baud 921600   — сменить скорость UART
//   !auto          — автопоиск скорости (115200/921600/1500000/2000000/460800)
//   !rx <gpio>     — какую ногу слушать (любая, кроме 18/19; напр. 6 или 20)
//   !info          — текущая скорость и счётчики
// Всё остальное уходит в UART как есть.

#include <Arduino.h>
#include "jtag2.h"

static bool logOn = true;

static int PIN_RX = 20;   // слушаемая нога; TX не используется (-1)
static const int PIN_LED = 8;   // синий LED, активный 0

static uint32_t baud = 115200;
static uint32_t rxBytes = 0;
static String cmd;
static bool inCmd = false;

static void openUart(uint32_t b) {
  Serial1.end();
  Serial1.setRxBufferSize(8192);
  pinMode(20, INPUT_PULLUP); pinMode(21, INPUT_PULLUP);   // RX камеры не должен висеть в воздухе
  Serial1.begin(b, SERIAL_8N1, PIN_RX, -1);
  baud = b;
}

// Оценка «читаемости» потока: доля печатных ASCII + \r\n
static int scoreBaud(uint32_t b, uint32_t ms) {
  openUart(b);
  uint32_t t0 = millis(), n = 0, good = 0;
  while (millis() - t0 < ms) {
    while (Serial1.available()) {
      int c = Serial1.read();
      n++;
      if ((c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n' || c == '\t') good++;
    }
  }
  if (n < 16) return -1;
  return (int)(good * 100 / n);
}

static void autoBaud() {
  static const uint32_t cand[] = {115200, 921600, 1500000, 2000000, 460800, 230400, 57600};
  Serial.println("[txw-prog] auto: включите/перезагрузите камеру несколько раз во время поиска");
  uint32_t best = 0; int bestScore = -1;
  for (uint32_t b : cand) {
    int s = scoreBaud(b, 3000);
    Serial.printf("[txw-prog]   %7u: %s%d%%\n", b, s < 0 ? "нет данных " : "", s < 0 ? 0 : s);
    if (s > bestScore) { bestScore = s; best = b; }
  }
  if (bestScore < 0) { Serial.println("[txw-prog] данных нет ни на одной скорости"); openUart(921600); return; }
  openUart(best);
  Serial.printf("[txw-prog] выбрано %u (%d%%)\n", best, bestScore);
}

static uint32_t num(const String& s) { return (uint32_t)strtoul(s.c_str(), nullptr, 0); }

static void jcmd(String c) {
  using namespace jtag2;
  String a[4]; int n = 0, from = 0;
  while (n < 4) { int sp = c.indexOf(' ', from); String t = sp < 0 ? c.substring(from) : c.substring(from, sp); t.trim();
    if (t.length()) a[n++] = t; if (sp < 0) break; from = sp + 1; }
  String k = a[0];
  if (k == "!log")   { logOn = num(a[1]); Serial.printf("[j] log=%d\n", logOn); }
  else if (k == "!jpins")  { PIN_HCK = num(a[1]); PIN_HDA = num(a[2]); if (n > 3) PIN_RST = num(a[3]);
                             Serial.printf("[j] HCK=%d HDA=%d RST=%d\n", PIN_HCK, PIN_HDA, PIN_RST); }
  else if (k == "!jprobe") { int pins[3] = {PIN_HCK, PIN_HDA, PIN_RST};
                             for (int i = 0; i < 3; i++) { int g = pins[i];
                               pinMode(g, INPUT_PULLUP); delay(5); int u = digitalRead(g);
                               pinMode(g, INPUT_PULLDOWN); delay(5); int d = digitalRead(g);
                               pinMode(g, INPUT);
                               Serial.printf("[j] GPIO%d: pullup->%d pulldown->%d => %s\n", g, u, d,
                                 (u == 1 && d == 0) ? "висит (никто не держит)" : (u == d ? (u ? "держат в 1" : "держат в 0") : "?")); } }
  else if (k == "!jcatch") { uint32_t ms = n > 1 ? num(a[1]) : 15000; uint32_t t0 = millis(), tries = 0; bool ok = false;
                             begin();
                             while (millis() - t0 < ms) { lineReset(60); uint32_t v = hadRead(HID); tries++;
                               if (v != 0xffffffff && v != 0) { Serial.printf("[j] CATCH t=%ums try=%u HID=0x%08x parityErr=%d\n", millis() - t0, tries, v, lastParityErr);
                                 uint32_t h = hadRead(HSR); Serial.printf("[j] HSR=0x%08x pe=%d\n", h, lastParityErr); ok = true; break; } }
                             if (!ok) Serial.printf("[j] catch: нет ответа за %u мс (%u попыток)\n", ms, tries); }
  else if (k == "!jbrute") { uint32_t ms = n > 1 ? num(a[1]) : 40000; uint32_t t0 = millis(), rounds = 0; bool ok = false;
                             const int G[4] = {4, 5, 6, 7};
                             while (!ok && millis() - t0 < ms) { rounds++;
                               for (int i = 0; i < 4 && !ok; i++) for (int j = 0; j < 4 && !ok; j++) { if (i == j) continue;
                                 for (int q = 0; q < 4; q++) pinMode(G[q], INPUT);
                                 PIN_HCK = G[i]; PIN_HDA = G[j]; PIN_RST = 3;   // RST на неподключённую ногу
                                 pinMode(PIN_HCK, OUTPUT); digitalWrite(PIN_HCK, HIGH); dirOut();
                                 lineReset(60); uint32_t v = hadRead(HID); int pe = lastParityErr;
                                 if (v != 0xffffffff && v != 0) { uint32_t v2 = hadRead(HID);
                                   Serial.printf("[j] HIT t=%ums HCK=GPIO%d HDA=GPIO%d HID=0x%08x pe=%d, повтор 0x%08x pe=%d\n",
                                                 millis() - t0, PIN_HCK, PIN_HDA, v, pe, v2, lastParityErr);
                                   if (v2 == v) ok = true; } } }
                             for (int q = 0; q < 4; q++) pinMode(G[q], INPUT);
                             if (!ok) Serial.printf("[j] brute: нет ответа за %u мс (%u кругов)\n", ms, rounds); }
  else if (k == "!dbg")    { bool ok = enterDebug(); Serial.printf("[j] debug=%d HSR=0x%08x\n", ok, hadRead(HSR)); }
  else if (k == "!csr")    { Serial.printf("[j] CSR=0x%08x\n", hadRead(CSR)); }
  else if (k == "!regr")   { uint8_t r = num(a[1]); uint32_t v = regRead(r); Serial.printf("[j] r%u=0x%08x\n", r, v); }
  else if (k == "!regw")   { uint8_t r = num(a[1]); uint32_t v = num(a[2]); regWrite(r, v); Serial.printf("[j] r%u<=0x%08x\n", r, v); }
  else if (k == "!mrd")    { uint32_t ad = num(a[1]); uint32_t cnt = n > 2 ? num(a[2]) : 4;
                             for (uint32_t i = 0; i < cnt; i++) { uint32_t v = cpuRead32(ad + 4 * i);
                               if (i % 4 == 0) Serial.printf("%s%08x:", i ? "\n" : "", ad + 4 * i);
                               Serial.printf(" %08x", v); }
                             Serial.printf("\n[j] retries=%u\n", readRetries); readRetries = 0; }
  else if (k == "!x")      { uint16_t ins = num(a[1]); execPlain(ins); Serial.printf("[j] exec 0x%04x HSR=0x%08x\n", ins, lastHsr); }
  else if (k == "!mwr")    { uint32_t ad = num(a[1]), v = num(a[2]); regWrite(1, ad); regWrite(2, v); execPlain(0xb140); execPlain(0xb140);
                             Serial.printf("[j] [0x%08x]<=0x%08x\n", ad, v); }
  else if (k == "!hcrrun") { hcrRun = num(a[1]); Serial.printf("[j] hcrRun=0x%x\n", hcrRun); }
  else if (k == "!csrrun") { csrRunOr = num(a[1]); Serial.printf("[j] csrRunOr=0x%x\n", csrRunOr); }
  else if (k == "!go")     { uint32_t pc = num(a[1]); uint32_t to = n > 2 ? num(a[2]) : 1000; int t = runTo(pc, to);
                             Serial.printf("[j] go 0x%08x -> %d ms, HSR=0x%08x PC=0x%08x\n", pc, t, hadRead(HSR), hadRead(PC)); }
  else if (k == "!wblk")   { // !wblk addr hexwords(без пробелов, по 8 символов)
                             uint32_t ad = num(a[1]); String h = a[2]; uint32_t nw = h.length() / 8; static uint32_t w[128];
                             if (nw > 128) nw = 128;
                             for (uint32_t i = 0; i < nw; i++) w[i] = strtoul(h.substring(i * 8, i * 8 + 8).c_str(), nullptr, 16);
                             blkWrite(ad, w, nw); static uint32_t r[128]; blkRead(ad, r, nw); uint32_t bad = 0;
                             for (uint32_t i = 0; i < nw; i++) if (r[i] != w[i]) bad++;
                             Serial.printf("[j] wblk 0x%08x n=%u bad=%u\n", ad, nw, bad); }
  else if (k == "!rblk")   { uint32_t ad = num(a[1]); uint32_t nw = num(a[2]); if (nw > 256) nw = 256; static uint32_t r[256];
                             blkRead(ad, r, nw); Serial.printf("[j] rblk 0x%08x ", ad);
                             for (uint32_t i = 0; i < nw; i++) Serial.printf("%08x", r[i]); Serial.println(); }
  else if (k == "!call")   { // !call fn a0 a1 a2 [timeout]
                             String b[7]; int m = 0, from = 0;
                             while (m < 7) { int sp = c.indexOf(' ', from); String t = sp < 0 ? c.substring(from) : c.substring(from, sp); t.trim();
                               if (t.length()) b[m++] = t; if (sp < 0) break; from = sp + 1; }
                             int ms = 0; uint32_t r0 = callFn(num(b[1]), num(b[2]), num(b[3]), num(b[4]), 0x20040000, 0x20003000, m > 5 ? num(b[5]) : 2000, &ms);
                             Serial.printf("[j] call 0x%08x -> r0=0x%08x %d ms PC=0x%08x\n", num(b[1]), r0, ms, hadRead(PC)); }
  else if (k == "!wblkf")  { // !wblkf addr <hex слов XOR 0x9E3779B9*(i+1)> s<сумма>
                             uint32_t ad = num(a[1]); String h = a[2]; uint32_t nw = h.length() / 8; static uint32_t w[128], rb[128];
                             if (nw > 128) nw = 128;
                             uint32_t sum = 0;
                             for (uint32_t i = 0; i < nw; i++) { w[i] = strtoul(h.substring(i * 8, i * 8 + 8).c_str(), nullptr, 16) ^ (0x9E3779B9u * (i + 1)); sum += w[i]; }
                             uint32_t want = a[3].length() > 1 ? strtoul(a[3].c_str() + 1, nullptr, 16) : 0;
                             if (sum != want || h.length() % 8) { Serial.printf("[j] wblkf ERR sum\n"); }
                             else {
                               blkWriteFast(ad, w, nw); blkReadFast(ad, rb, nw); uint32_t bad = 0;
                               for (uint32_t i = 0; i < nw; i++) if (rb[i] != w[i]) bad++;
                               uint32_t hid = hadRead(HID);
                               if (hid != 0x14039322) { lineReset(); bad = 9999; }
                               Serial.printf("[j] wblkf 0x%08x n=%u bad=%u\n", ad, nw, bad);
                             } }
  else if (k == "!rblkf")  { uint32_t ad = num(a[1]); uint32_t nw = num(a[2]); if (nw > 256) nw = 256; static uint32_t r[256];
                             blkReadFast(ad, r, nw);
                             static char ob[2200]; int L;
                             uint32_t hid = hadRead(HID);
                             if (hid != 0x14039322) {          // линия сбилась — данные негодны
                               lineReset(); L = snprintf(ob, sizeof ob, "[j] rblkf ERR hid=0x%08x", hid);
                             } else {
                               // слова XOR-ятся маской (длинные серии одинаковых символов теряются в USB), + сумма
                               L = snprintf(ob, sizeof ob, "[j] rblkf 0x%08x ", ad); uint32_t sum = 0;
                               for (uint32_t i = 0; i < nw; i++) { sum += r[i];
                                 L += snprintf(ob + L, sizeof ob - L, "%08x", r[i] ^ (0x9E3779B9u * (i + 1))); }
                               L += snprintf(ob + L, sizeof ob - L, " s%08x", sum);
                             }
                             ob[L++] = '\n';
                             for (int o = 0, tries = 0; o < L && tries < 2000; tries++) {   // дописать всё до конца
                               int w = Serial.write((const uint8_t*)ob + o, L - o); o += w; if (w <= 0) delay(1); }
                             }
  else if (k == "!jnop")   { nopLoops = num(a[1]); Serial.printf("[j] nop=%u%c", nopLoops, 10); }
  else if (k == "!jinit")  { begin(); lineReset(); Serial.printf("[j] init: HCK=GPIO%d HDA=GPIO%d RST=GPIO%d, линия сброшена\n", PIN_HCK, PIN_HDA, PIN_RST); }
  else if (k == "!jrel")   { release(); Serial.println("[j] линии отпущены"); }
  else if (k == "!jreset") { lineReset(n > 1 ? num(a[1]) : 80); Serial.println("[j] line reset"); }
  else if (k == "!jspeed") { halfUs = num(a[1]); Serial.printf("[j] halfUs=%u\n", halfUs); }
  else if (k == "!jidle")  { idleClk = num(a[1]); Serial.printf("[j] idle=%u\n", idleClk); }
  else if (k == "!jpad")   { padTo32 = num(a[1]); Serial.printf("[j] pad32=%d\n", padTo32); }
  else if (k == "!hid")    { uint32_t v = hadRead(HID); Serial.printf("[j] HID=0x%08x parityErr=%d\n", v, lastParityErr); }
  else if (k == "!hr")     { uint8_t rs = num(a[1]); uint8_t b = n > 2 ? num(a[2]) : 32;
                             uint32_t v = hadRead(rs, b); Serial.printf("[j] HAD[0x%02x]=0x%08x parityErr=%d\n", rs, v, lastParityErr); }
  else if (k == "!hw")     { uint8_t rs = num(a[1]); uint32_t v = num(a[2]); hadWrite(rs, v); Serial.printf("[j] HAD[0x%02x]<=0x%08x\n", rs, v); }
  else if (k == "!rd")     { uint32_t ad = num(a[1]); uint32_t cnt = n > 2 ? num(a[2]) : 4;
                             for (uint32_t i = 0; i < cnt; i++) { uint32_t v = memRead32(ad + 4 * i);
                               if (i % 4 == 0) Serial.printf("%s%08x:", i ? "\n" : "", ad + 4 * i);
                               Serial.printf(" %08x%s", v, lastParityErr ? "!" : ""); }
                             Serial.println(); }
  else if (k == "!rst")    { if (a[1] == "z") pinMode(PIN_RST, INPUT); else { pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, num(a[1])); }
                             Serial.printf("[j] RST=%s\n", a[1].c_str()); }
  else Serial.println("[j] команды: !jinit !jrel !jreset [n] !jspeed us !jidle n !jpad 0|1 !hid !hr rs [bits] !hw rs val !rd addr [n] !rst 0|1|z !log 0|1");
}

static void handleCmd(String c) {
  c.trim();
  if (c.startsWith("!baud")) {
    uint32_t b = c.substring(5).toInt();
    if (b >= 1200) { openUart(b); Serial.printf("[txw-prog] baud=%u\n", b); }
    else Serial.println("[txw-prog] ошибка: !baud <скорость>");
  } else if (c.startsWith("!rx")) {
    int p = c.substring(3).toInt();
    // любая GPIO, кроме USB (18/19)
    if (p >= 0 && p <= 21 && p != 18 && p != 19) { PIN_RX = p; openUart(baud); Serial.printf("[txw-prog] rx=GPIO%d\n", p); }
    else Serial.println("[txw-prog] ошибка: !rx <GPIO 0..21, кроме 18/19>");
  } else if (c.startsWith("!j") || c.startsWith("!h") || c.startsWith("!rd") || c.startsWith("!rst") || c.startsWith("!dbg") || c.startsWith("!csr") || c.startsWith("!reg") || c.startsWith("!mrd") || c.startsWith("!x") || c.startsWith("!mwr") || c.startsWith("!go") || c.startsWith("!csrrun") || c.startsWith("!hcr") || c.startsWith("!wblk") || c.startsWith("!rblk") || c.startsWith("!call") || c.startsWith("!log")) {
    jcmd(c);
  } else if (c == "!auto") {
    autoBaud();
  } else if (c == "!up") {
    Serial.printf("[j] up=%lu ms reset=%d\n", millis(), (int)esp_reset_reason());
  } else if (c == "!info") {
    Serial.printf("[txw-prog] pin=GPIO%d baud=%u rx=%u\n", PIN_RX, baud, rxBytes);
  } else {
    Serial.println("[txw-prog] команды: !rx <gpio>, !baud N, !auto, !info");
  }
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  Serial.setRxBufferSize(4096);
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);  // USB CDC, скорость не важна
  openUart(baud);
  delay(300);
  Serial.printf("[txw-prog] мост UART GPIO20 -> USB, baud=%u. Команды: !baud N, !auto, !info\n", baud);
}

void loop() {
  // UART -> USB
  uint8_t buf[256];
  int n = Serial1.available();
  if (n > 0) {
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    n = Serial1.readBytes(buf, n);
    if (logOn) Serial.write(buf, n);
    rxBytes += n;
    digitalWrite(PIN_LED, (rxBytes >> 6) & 1);
  }
  // USB -> команды / UART
  while (Serial.available()) {
    char c = Serial.read();
    if (!inCmd && c == '!') { inCmd = true; cmd = "!"; continue; }
    if (inCmd) {
      if (c == '\n' || c == '\r') { handleCmd(cmd); inCmd = false; cmd = ""; }
      else if (cmd.length() < 2200) cmd += c;
    } else {
      (void)c;  // передача в камеру отключена
    }
  }
}
