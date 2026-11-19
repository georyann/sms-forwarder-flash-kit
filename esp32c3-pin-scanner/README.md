# ESP32-C3 引脚自检工具

用在**一体机**（ESP32-C3 + ML307A 焊在同一块 PCB）上，自动找出 ESP32-C3 与 ML307A
之间的 UART 引脚。**不用拆机追线，不用焊接。**

> 💡 不想敲命令的话，用上级目录的 **`../一键刷机.sh`**：
> 菜单里选 `[4] 烧录引脚自检固件` 会自动编译烧录，选 `[5] 打开扫引脚控制台` 直接进串口。
> 本 README 是这套工具的原理与手动操作说明。

## ✅ 实测结果（2026-11-19，本机已验证）

在这台一体机上实际扫出来的接线如下，开源固件直接用这组引脚即可：

| 方向 | GPIO | 说明 |
|---|---|---|
| ESP32-C3 **TXD** → 模组 RX | **GPIO3** | 固件 `#define TXD 3` |
| 模组 TX → ESP32-C3 **RXD** | **GPIO4** | 固件 `#define RXD 4` |
| 模组 EN / 电源控制 | **无** | GPIO5 是浮空的，模组常电供电，**没有可控制的 EN 脚** |

结论：**固件里的 `#define TXD 3` / `#define RXD 4` 与实物一致，不用改。**
`#define MODEM_EN_PIN 5` 指向的是一根空脚，开机时会被拉高，写高电平对模组无影响，
属于「无害但也无效」的写法，留着即可（改成任意空闲 GPIO 也一样）。

> ⚠️ 因为没有可控制的 EN 脚，**没法用软件给模组断电重启**。如果模组假死，
> 只能给整机断电重上电。这不影响日常使用（模组自带回电恢复）。

刷完开源固件后的实测状态：开机正常联网、SIM 注册成功（联通，信号 -46 dBm）、
Bark 推送返回 HTTP 200，说明 UART 接线没问题。

## 为什么需要它

刷开源固件时，开源项目 `maxming2333/esp32-sms-forwarding` 的串口引脚是编译期写死的：

```cpp
#define TXD 3          // ESP32-C3 GPIO3 → 模组 RX
#define RXD 4          // 模组 TX → ESP32-C3 GPIO4
#define MODEM_EN_PIN 5 // 模组 EN 供电控制
```

一体机厂商完全可能换用别的引脚。与其拆机用万用表量 PCB 走线（PCB 走线藏在下层，
容易量错还得返工），不如让 ESP32-C3 自己扫一遍 —— 它的 UART 通过 GPIO Matrix 可以
映射到任意 GPIO，暴力搜索是可靠的。

## 工作原理

- ML307A 主串口默认 **115200 8N1**，对 `AT` 指令回 `OK`
- **扫 RX**：把每个候选 GPIO 配成 UART 接收口听 1.2 秒。期间给 SIM 卡发一条短信，
  模组会吐 `+CMTI:` URC，能捕到可打印文本流的那根脚就是 RX
- **扫 TX**：固定 RX，每个候选脚发一次 `AT\r\n`，看 RX 上有没有回 `OK`
- **扫 EN**：固定 RX，每个候选脚拉低 1.2 秒再拉高，看模组是否重启（串口上出现
  `RDY` 开机串）

## 使用步骤

### 1. 备份原厂固件（重要）

刷任何东西之前先备份，否则出事只能拆机短接 BOOT 救砖：

```bash
pip install "esptool>=4.8"
esptool --chip esp32c3 --port /dev/tty.usbmodem* read_flash 0x0 0x400000 factory_backup.bin
```

### 2. 编译

```bash
pio run
```

产物：`.pio/build/makergo_esp32c3_supermini/firmware.bin`

### 3. 烧录

**必须用 PlatformIO 烧录**：

```bash
pio run -t upload
```

> ⚠️ **不要手工拼镜像再 esptool 写入 `0x0`。**
> 本目录里曾经放过一个手工合并的 `scanner_full.bin`，改过镜像头，
> ROM 的 SHA-256 校验对不上，表现为「刷完开不了机、只能短接 BOOT 救砖」。
> 那个文件已废弃（保留在这里只为提醒），正确做法永远是 `pio run -t upload`：
> 它会按顺序写 bootloader(0x0) → partitions(0x8000) → app(0x10000) → LittleFS 网页。

用 Chrome / Edge 打开 [ESPConnect](https://thelastoutpostworkshop.github.io/ESPConnect/)，
波特率选 `460800`，连接时选 `USB JTAG/serial debug unit`，进「闪存工具」也能烧，
但地址和偏移要按上表分别填，不能合并成一个文件。

> 一体机如果没有引出 BOOT/RST 按钮，需要按住 BOOT 再插 USB，或短接两个测试点进下载模式。

### 4. 打开串口监视器

波特率 **115200**（这是 USB CDC 控制台，跟模组波特率无关）。

### 5. 依次执行命令

```
> s
```
扫描接收引脚。**扫描开始后立刻给自己的 SIM 卡发一条短信**（模组收到会吐 URC，
扫描才能捕获到）。约 20 秒。

输出里带 `<<< 候选 RX！` 的就是候选。可能有多个，用下面命令逐个确认。

```
> m 4
```
监听 GPIO4。如果能看到 `+CMTI:` / `+CREG:` / `RDY` 之类的 AT 文本，就是它。

```
> f 4
```
已知 RX=GPIO4，暴力搜索 TX。约 15 秒。输出里带 `<<< 命中 TX！` 的那行，
GPIO 号就是 ESP32 侧的 TX。

```
> e 4
```
已知 RX，暴力搜索 EN 引脚。约 70 秒（每个脚要等模组重启）。

```
> t 4 3 AT+CGMR
```
验证组合：往 GPIO3 发 `AT+CGMR`，应该返回模组型号（含 ML307 字样）。

```
> t 4 3 AT+CSQ
```
返回信号强度，第一格是 `+CSQ: <rssi>,<ber>`。

### 6. 改开源固件的引脚

拿到 GPIO 号后，改开源固件 `esp32-sms-forwarding` 的 `src/main.cpp`：

```cpp
#define TXD <你的TX号>          // 本机实测：3
#define RXD <你的RX号>          // 本机实测：4
#define MODEM_EN_PIN <你的EN号> // 本机实测：无 EN 脚，GPIO5 是空脚，写什么都行
```

重新编译烧录（一条命令，网页文件 LittleFS 也会一起烧进去）：

```bash
pio run -t upload
```

### 7. 首次配置

1. 上电后未配网会开热点 `SMS-Forwarder-AP`（无密码）
2. 连上，浏览器访问 `http://192.168.4.1`
3. 填 WiFi SSID / 密码，保存，设备自动重启
4. 重启后用路由器分配的 IP 访问后台
5. 默认账号密码 **`admin / admin123`**，立刻改掉
6. 在推送通道区配置 Bark / 钉钉 / 飞书 / 邮件等

## 排除的 GPIO

| GPIO | 原因 |
|---|---|
| 12–17 | **接的是 SPI Flash**，复用会直接让芯片变砖（之前踩过：日志 `rst:0x8 TG1WDT_SYS_RST`） |
| 18, 19 | USB D-/D+，占用会导致串口失联、无法烧录 |
| 9 | BOOT 按键，拉低会进下载模式 |
| 8 | 板载 LED（Super Mini） |
| 2 | 启动绑带脚，置高会导致之后无法进下载模式 |
| 5 | 本机是空脚（无 EN 接线），可以随便用 |

## 备选方案：直接量线

如果不想刷自检固件，也可以物理确认：

- ML307A 模组主串口 **pin 7 = UART1_TXD**（接 ESP32 的 RX）、**pin 8 = UART1_RXD**（接 ESP32 的 TX）
- **pin 15 = PWRKEY**（拉低 1.2 秒开机）、**pin 16 = RESET**（低有效，拉低 ≥50ms）
- **pin 18 = NET_LED**（网络状态指示）
- 一体机若把 EN 短接到 5V（常电开机，DIY 方案常见），那就没有可控制的 EN，
  开源固件的 `MODEM_EN_PIN` 需改成一个空闲脚或用飞线补接

万用表量「通断」档，一端接模组 pin 7，另一端逐个碰 ESP32-C3 的焊盘，
通了的那根就是 TX 脚（模组 TX → ESP32 RX）。注意同一条线上电压要一致。
