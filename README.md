# 短信转发器 · 一键刷机工具包

ESP32-C3 + ML307A（Cat.1 4G）短信转发一体机：把原厂闭源固件换成自研开源固件，
并且全程「傻瓜式」——网页里点一下就能配网、填推送通道。

## 目录结构

| 路径 | 说明 |
|---|---|
| `一键刷机.sh` | 菜单式刷机助手（烧固件 / 备份原厂 / 恢复原厂 / 烧引脚自检 / 查状态 / 开网页） |
| `使用说明.md` | 日常使用手册（配网、推送通道、收件箱、到期提醒） |
| `firmware/` | 自研固件源码（PlatformIO 工程，网页在 `firmware/data/`） |
| `esp32c3-pin-scanner/` | 引脚自检工程（排查模组接线用） |
| `backup/` | 原厂固件备份目录（4MB 二进制，不入库） |

## 上手三步

```bash
./一键刷机.sh 备份       # 1. 先备份原厂闭源固件（后悔药）
./一键刷机.sh 刷固件     # 2. 编译并烧录自研固件（会一并写入网页文件）
./一键刷机.sh 状态       # 3. 查设备 IP 和健康状态
```

烧完后手机/电脑连上和设备同一个 WiFi，浏览器打开 `http://<设备IP>/`，
默认账号 `admin` / `admin123`（**请立刻改掉**）。

换到新环境配网：连设备自己的热点 `SMS-Forwarder-01CC9C`（密码 `smsfw1234`），
浏览器打开 `http://192.168.4.1/`，在网页里扫描并选择新环境的 WiFi。

## 固件源码与上游

固件基于开源项目 [maxming2333/esp32-sms-forwarding](https://github.com/maxming2333/esp32-sms-forwarding)
改造，新增了：

- 设备热点 SSID / 密码自定义，支持「热点常开」（换环境配网兜底）
- 工具箱「📨 收到的信息」收件箱：LittleFS 保存最近 50 条短信与来电
- 工具箱「📅 SIM 卡到期提醒」：任意号码 + 提前 30/15/7/1 天各推一次续费提醒
- 修掉整机时间比真实时间差 8 小时的问题（`AT+CCLK?` 的 UTC/时区处理）

对应的 GitHub 仓库：

- 工具包本仓库：<https://github.com/georyann/sms-forwarder-flash-kit>
- 上游 fork（含上游完整历史 + 固件改动 + `tools/` 一键刷机）：<https://github.com/georyann/esp32-sms-forwarding>

## 注意事项

- 固件必须用 PlatformIO 烧录（`pio run -t upload` 会依次写 bootloader、分区表、
  app 和 LittleFS 网页文件）。**不要手工拼 bin 再 esptool 写 `0x0`**，
  镜像头被改过会被 ROM 校验拒绝，表现为「刷完开不了机」。
- 每次 `pio run -t upload` 都会重写 LittleFS 网页分区，收件箱里的历史记录会被清空。
- ML307A 模组常电供电且没有可控 EN 脚，固件无法软件复位模组，模组假死只能整机断电。
