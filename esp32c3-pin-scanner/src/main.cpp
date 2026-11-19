/*
 * ESP32-C3 引脚自检固件 (Pin Scanner)
 * -------------------------------------------------------------------------
 * 用途：在一体机（ESP32-C3 + ML307A 焊在同一块 PCB 上）上自动找出
 *       ESP32-C3 与 ML307A 之间的 UART 引脚，无需拆机追线、无需焊接。
 *
 * 原理：
 *   - ML307A 的 UART 默认 115200 8N1，对 AT 指令会回 "OK"
 *   - ESP32-C3 有 GPIO Matrix，UART 的 TX/RX 可映射到任意 GPIO
 *   - 因此可以「听」和「说」双向暴力搜索，找到唯一能收发 AT 的那对引脚
 *
 * 使用方法（串口监视器 115200）：
 *   s          扫描接收引脚（需在扫描期间给 SIM 卡发一条短信，触发 +CMTI URC）
 *   f <rx>     已知 RX，暴力搜索发送引脚
 *   e <rx>     已知 RX，暴力搜索 EN/电源控制引脚（脉冲低电平看模组是否重启）
 *   m <rx> <tx> 实时监听模组串口全部数据
 *   t <rx> <tx> <文本>  发送一条 AT 指令并打印响应
 *   q          退出
 *
 * 排除的 GPIO：
 *   18, 19  USB D-/D+，占用会导致串口失联
 *   9       BOOT 按键，拉低会进下载模式
 *   8       板载 LED
 *   2       启动绑带脚，置高会导致之后无法进下载模式
 */

#include <Arduino.h>

// ML307A 主串口默认波特率；其他常见值可用 "b <rate>" 切换
static uint32_t g_baud = 115200;

// 候选引脚。0 保留（下载绑带脚，但正常工作无碍）；20/21 是 ESP32-C3
// 的默认 UART0 引脚，模组若接在上面也可能被扫到。
static const int kCand[] = {0, 1, 3, 4, 5, 6, 7, 10, 11, 12, 13,
                            14, 15, 16, 17, 20, 21};
static const int kCandCount = sizeof(kCand) / sizeof(kCand[0]);

// 用 UART1 走模组，UART0(USB CDC) 做控制台，两者互不干扰
HardwareSerial Modem(1);

static void console(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
  Serial.flush();
}

// 统计一段数据里有多少个「可打印 ASCII 字符」，用来判断这根线上
// 有没有在跑真正的串口协议（AT 响应都是可打印文本）。
static int printableCount(const uint8_t *d, size_t n) {
  int c = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t b = d[i];
    if ((b >= 0x20 && b <= 0x7E) || b == '\r' || b == '\n' || b == '\t') c++;
  }
  return c;
}

// 扫描「接收」引脚：把候选脚配成 UART RX 听一会儿，
// 看有没有 AT 协议的文本流过。
static void scanRx() {
  console("\n=== 扫描接收引脚 (RX) ===\n");
  console("现在请立刻给设备里的 SIM 卡发一条短信！\n");
  console("（模组收到短信会从 TX 吐 +CMTI: URC，扫描会捕获到）\n\n");

  for (int i = 0; i < kCandCount; i++) {
    int p = kCand[i];

    Modem.end();
    Modem.begin(g_baud, SERIAL_8N1, p, -1);  // -1 = 不接 TX，纯监听

    uint8_t buf[512];
    size_t total = 0;
    // 每个引脚听 1200ms
    unsigned long t0 = millis();
    while (millis() - t0 < 1200) {
      if (Modem.available()) {
        size_t n = Modem.readBytes(buf, sizeof(buf));
        for (size_t j = 0; j < n && total < sizeof(buf); j++) buf[total++] = buf[j];
      }
      delay(5);
    }

    int printable = printableCount(buf, total);
    // 可打印字符占比高，且有一定数量 → 这根线在跑串口
    bool isSerial = (total > 0) && (printable * 100 / total > 70) && (printable > 8);

    console("GPIO %-3d  bytes=%-5u printable=%-5d  %s", p, total, printable,
            isSerial ? "  <<< 候选 RX！" : "");
    if (isSerial) {
      console("  内容: ");
      for (size_t j = 0; j < total && j < 160; j++) {
        uint8_t b = buf[j];
        if (b == '\r') continue;
        console("%c", (b >= 0x20 && b <= 0x7E) ? b : '.');
      }
    }
    console("\n");
  }
  Modem.end();
  console("\n扫描结束。若有多个候选，请用 'm <rx>' 逐个监听确认。\n");
}

// 已知 RX，暴力搜索 TX：每个候选脚发 AT，看 RX 上有没有回 OK。
static void scanTx(int rx) {
  console("\n=== 搜索发送引脚 (TX)，RX 固定为 GPIO %d ===\n", rx);
  console("波特率 %u\n\n", (unsigned)g_baud);

  for (int i = 0; i < kCandCount; i++) {
    int p = kCand[i];
    if (p == rx) continue;

    Modem.end();
    Modem.begin(g_baud, SERIAL_8N1, rx, p);

    // 先把接收侧积压的垃圾清掉
    unsigned long t0 = millis();
    while (Modem.available() && millis() - t0 < 50) { Modem.read(); }

    Modem.print("AT\r\n");

    uint8_t buf[256];
    size_t total = 0;
    t0 = millis();
    while (millis() - t0 < 600) {
      if (Modem.available()) {
        size_t n = Modem.readBytes(buf, sizeof(buf));
        for (size_t j = 0; j < n && total < sizeof(buf); j++) buf[total++] = buf[j];
      }
      delay(5);
    }

    bool gotOk = false;
    for (size_t j = 0; j + 2 <= total; j++) {
      if (buf[j] == 'O' && buf[j + 1] == 'K' &&
          (buf[j + 2] == '\r' || buf[j + 2] == '\n')) { gotOk = true; break; }
    }

    console("GPIO %-3d -> RX %d : %s", p, rx, gotOk ? "收到 OK  <<< 命中 TX！" : "无响应");
    if (total) {
      console("  [");
      for (size_t j = 0; j < total && j < 80; j++) {
        uint8_t b = buf[j];
        if (b == '\r' || b == '\n') continue;
        console("%c", (b >= 0x20 && b <= 0x7E) ? b : '.');
      }
      console("]");
    }
    console("\n");
  }
  Modem.end();
  console("\n搜索结束。命中的 GPIO 就是 ESP32 侧的 TX（接模组 RX）。\n");
}

// 已知 RX，暴力搜索 EN/电源控制脚：拉低 1.2 秒再拉高，
// 若模组重启，串口上会出现一串开机 URC（RDY / +CPIN: / +CREG:）。
static void scanEn(int rx) {
  console("\n=== 搜索 EN/电源控制引脚，RX 固定为 GPIO %d ===\n", rx);
  console("每个引脚会短暂被拉低 1.2 秒，请勿断电。\n\n");

  for (int i = 0; i < kCandCount; i++) {
    int p = kCand[i];
    if (p == rx) continue;

    Modem.end();
    Modem.begin(g_baud, SERIAL_8N1, rx, -1);

    // 清空缓冲
    unsigned long t0 = millis();
    while (Modem.available() && millis() - t0 < 50) { Modem.read(); }

    // 脉冲：拉低 -> 拉高（模组 EN 一般高电平使能）
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
    delay(1200);
    digitalWrite(p, HIGH);
    pinMode(p, INPUT);  // 交回给模组驱动，避免长期强拉

    uint8_t buf[1024];
    size_t total = 0;
    t0 = millis();
    while (millis() - t0 < 4000) {
      if (Modem.available()) {
        size_t n = Modem.readBytes(buf, sizeof(buf));
        for (size_t j = 0; j < n && total < sizeof(buf); j++) buf[total++] = buf[j];
      }
      delay(5);
    }

    bool sawRdy = false;
    for (size_t j = 0; j + 3 <= total; j++) {
      if (buf[j] == 'R' && buf[j+1] == 'D' && buf[j+2] == 'Y') { sawRdy = true; break; }
    }

    console("GPIO %-3d : bytes=%-5u %s", p, total, sawRdy ? "  <<< 模组重启！这是 EN" : "");
    if (total) {
      console("  [");
      for (size_t j = 0; j < total && j < 100; j++) {
        uint8_t b = buf[j];
        if (b == '\r' || b == '\n') continue;
        console("%c", (b >= 0x20 && b <= 0x7E) ? b : '.');
      }
      console("]");
    }
    console("\n");
  }
  Modem.end();
}

// 实时监听
static void monitor(int rx, int tx) {
  console("\n=== 监听 GPIO%d(RX) / GPIO%d(TX)，Ctrl+C 退出 ===\n", rx, tx);
  Modem.end();
  Modem.begin(g_baud, SERIAL_8N1, rx, tx);
  uint8_t buf[256];
  while (true) {
    if (Modem.available()) {
      size_t n = Modem.readBytes(buf, sizeof(buf));
      console("[RX] ");
      for (size_t j = 0; j < n; j++) {
        uint8_t b = buf[j];
        if (b == '\r') continue;
        if (b == '\n') console("\n");
        else console("%c", (b >= 0x20 && b <= 0x7E) ? b : '.');
      }
      console("\n");
    }
    delay(2);
  }
}

// 发一条 AT 指令并打印响应
static void sendAt(int rx, int tx, const char *text) {
  Modem.end();
  Modem.begin(g_baud, SERIAL_8N1, rx, tx);
  unsigned long t0 = millis();
  while (Modem.available() && millis() - t0 < 50) { Modem.read(); }

  Modem.print(text);
  Modem.print("\r\n");

  uint8_t buf[512];
  size_t total = 0;
  t0 = millis();
  while (millis() - t0 < 1500) {
    if (Modem.available()) {
      size_t n = Modem.readBytes(buf, sizeof(buf));
      for (size_t j = 0; j < n && total < sizeof(buf); j++) buf[total++] = buf[j];
    }
    delay(5);
  }
  console(">>> %s\n", text);
  for (size_t j = 0; j < total; j++) {
    uint8_t b = buf[j];
    if (b == '\r') continue;
    console("%c", b == '\n' ? '\n' : ((b >= 0x20 && b <= 0x7E) ? b : '.'));
  }
  console("\n");
}

// 逐字节读一行（Arduino 的 HardwareSerial 没有 readLineUntil，这里自己实现）
static bool readLine(char *out, size_t cap, unsigned long timeoutMs) {
  size_t i = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n') {
        out[i] = 0;
        return true;
      }
      if (c == '\r') continue;
      if (i + 1 < cap) out[i++] = c;
    }
    delay(2);
  }
  out[i] = 0;
  return i > 0;
}

static void printHelp() {
  console("\n===== ESP32-C3 引脚自检 =====\n");
  console("s            扫描接收引脚 RX（需同时给 SIM 发短信）\n");
  console("f <rx>       搜索发送引脚 TX\n");
  console("e <rx>       搜索 EN/电源控制引脚\n");
  console("m <rx> <tx>  实时监听模组串口\n");
  console("t <rx> <tx> <文本>  发送 AT 指令\n");
  console("b <波特率>   切换波特率（默认 115200）\n");
  console("q            退出\n");
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) { delay(10); }
  delay(300);

  console("\n\n=== ESP32-C3 引脚自检固件 v1.0 ===\n");
  console("目标: 找出 ESP32-C3 与 ML307A 之间的 UART 引脚\n");
  console("候选 GPIO:");
  for (int i = 0; i < kCandCount; i++) console(" %d", kCand[i]);
  console("\n");
  printHelp();

  char line[128];
  while (true) {
    console("\n> ");
    if (!readLine(line, sizeof(line), 10000)) continue;
    // 去掉 \r 和首尾空格
    char *s = line;
    while (*s == ' ') s++;
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\r' || s[l-1] == ' ')) s[--l] = 0;

    if (s[0] == 0) continue;

    if (s[0] == 's' && s[1] == 0) {
      scanRx();
    } else if (s[0] == 'f' && s[1] == ' ') {
      scanTx(atoi(s + 2));
    } else if (s[0] == 'e' && s[1] == ' ') {
      scanEn(atoi(s + 2));
    } else if (s[0] == 'm' && s[1] == ' ') {
      int rx = 0, tx = 0;
      if (sscanf(s + 2, "%d %d", &rx, &tx) == 2) monitor(rx, tx);
    } else if (s[0] == 't' && s[1] == ' ') {
      int rx = 0, tx = 0;
      char text[100] = {0};
      int n = sscanf(s + 2, "%d %d %[^\n]", &rx, &tx, text);
      if (n >= 3) sendAt(rx, tx, text);
      else console("用法: t <rx> <tx> <文本>\n");
    } else if (s[0] == 'b' && s[1] == ' ') {
      g_baud = (uint32_t)atol(s + 2);
      console("波特率已设为 %u\n", (unsigned)g_baud);
    } else if (s[0] == 'q') {
      console("再见。\n");
      return;
    } else {
      console("未知命令。输入 h 看帮助。\n");
    }
    printHelp();
  }
}

void loop() {}
