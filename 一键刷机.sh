#!/bin/bash
# ==============================================================================
#  短信转发器 一键刷机助手
#  ESP32-C3 + ML307A 一体机（自研开源固件）
#
#  用法（全部可选，不用记参数）：
#    ./一键刷机.sh              交互式菜单，选你要做什么
#    ./一键刷机.sh 刷固件       编译并烧录自研开源固件（最常用）
#    ./一键刷机.sh 备份         备份原厂闭源固件
#    ./一键刷机.sh 恢复原厂     把原厂闭源固件刷回去
#    ./一键刷机.sh 刷扫描器     烧录引脚自检固件（排查接线用）
#    ./一键刷机.sh 扫引脚       打开串口交互界面
#    ./一键刷机.sh 状态         查看设备 IP 并做健康检查
#
#  说明：固件必须用 PlatformIO 烧录（会同时写入 bootloader、分区表、app 和
#        LittleFS 网页文件）。不要手工拼 bin 再 esptool 写入 —— 手工改过
#        镜像头的文件会被 ROM 校验拒绝，表现为「刷完开不了机」。
# ==============================================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 两种布局都支持：
#   A) 独立工具包： 一键刷机.sh 与 firmware/、esp32c3-pin-scanner/ 同级
#   B) 仓库内 tools/：上一级目录就是固件工程（platformio.ini 在那里）
if [ -f "$SCRIPT_DIR/platformio.ini" ]; then
  FIRMWARE_DIR="$SCRIPT_DIR"
  SCANNER_DIR="$SCRIPT_DIR/esp32c3-pin-scanner"
elif [ -f "$SCRIPT_DIR/../platformio.ini" ]; then
  FIRMWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
  SCANNER_DIR="$SCRIPT_DIR/esp32c3-pin-scanner"
else
  FIRMWARE_DIR="$SCRIPT_DIR/firmware"
  SCANNER_DIR="$SCRIPT_DIR/esp32c3-pin-scanner"
fi

BACKUP_DIR="$SCRIPT_DIR/backup"
BACKUP_FILE="$BACKUP_DIR/factory_backup.bin"
STATE_FILE="$SCRIPT_DIR/.last_ip"

# PlatformIO 装在固定位置，重启电脑也不会丢
PIO_VENV="$HOME/tools/piovenv"
PIO_BIN="$PIO_VENV/bin/pio"
PY_BIN="$PIO_VENV/bin/python"
ESPTOOL="$HOME/.platformio/packages/tool-esptool/esptool"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

say()  { echo -e "${GREEN}$*${NC}"; }
warn() { echo -e "${YELLOW}$*${NC}"; }
err()  { echo -e "${RED}$*${NC}"; }
info() { echo -e "${BLUE}$*${NC}"; }
step() { echo -e "\n${BOLD}$*${NC}"; }

# ---------------------------------------------------------------- 找串口
find_port() {
  # 优先用系统已枚举的 usbmodem 设备
  local p
  for p in /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/tty.usbmodem* /dev/tty.usbserial-*; do
    [ -e "$p" ] || continue
    # 确认它是 Espressif 的设备（避免选到别的 USB 串口）
    if ioreg -p IOUSB -l -w 0 2>/dev/null | grep -qi "Espressif"; then
      echo "$p"; return 0
    fi
  done
  # 兜底：用 esptool 自己探测
  if [ -x "$ESPTOOL" ]; then
    for p in /dev/cu.usbmodem* /dev/cu.usbserial-* /dev/tty.usbmodem* /dev/tty.usbserial-*; do
      [ -e "$p" ] || continue
      if "$ESPTOOL" --chip esp32c3 --port "$p" chip_id >/dev/null 2>&1; then
        echo "$p"; return 0
      fi
    done
  fi
  return 1
}

# 串口读取要用 callout 设备（/dev/cu.*），esptool 两边都能用
read_port() {
  case "$1" in
    /dev/cu.*) echo "$1" ;;
    /dev/tty.*) echo "/dev/cu.${1#/dev/tty.}" ;;
    *) echo "$1" ;;
  esac
}

# ---------------------------------------------------------------- 环境准备
ensure_pio() {
  if [ -x "$PIO_BIN" ]; then return 0; fi
  warn "没有找到 PlatformIO，正在安装（只需一次，约 2 分钟）…"
  mkdir -p "$(dirname "$PIO_VENV")"
  python3 -m venv "$PIO_VENV" 2>/dev/null || {
    err "创建 Python 虚拟环境失败，请检查 python3 是否可用"
    exit 1
  }
  "$PIO_VENV/bin/pip" install --quiet --upgrade pip
  "$PIO_VENV/bin/pip" install --quiet platformio
  if [ ! -x "$PIO_BIN" ]; then
    err "PlatformIO 安装失败，请手动执行： $PIO_VENV/bin/pip install platformio"
    exit 1
  fi
  say "PlatformIO 安装完成"
}

ensure_esptool() {
  if [ -x "$ESPTOOL" ]; then return 0; fi
  ensure_pio
  if "$PIO_VENV/bin/pip" install --quiet esptool 2>/dev/null; then
    ESPTOOL="$PIO_VENV/bin/esptool"
    return 0
  fi
  err "esptool 安装失败"
  exit 1
}

check_device() {
  ensure_esptool
  step "检查设备连接"
  PORT="$(find_port)" || true
  if [ -z "${PORT:-}" ]; then
    err "没找到设备！请检查："
    echo "  1. USB 线是否插好（必须是数据线，不能是纯充电线）"
    echo "  2. 一体机是否已上电（有些板子需要单独按电源键）"
    echo "  3. 换个 USB 口或直连 Mac（尽量不要经过 hub）"
    exit 1
  fi
  say "找到设备：$PORT"
  "$ESPTOOL" --chip esp32c3 --port "$PORT" chip_id 2>/dev/null \
    | grep -E "Chip type|Embedded Flash|MAC" | sed 's/^/    /'
  echo
}

# ---------------------------------------------------------------- 备份 / 恢复
backup() {
  ensure_esptool
  local port="$1"
  mkdir -p "$BACKUP_DIR"
  if [ -f "$BACKUP_FILE" ]; then
    warn "已存在备份文件，跳过。删除它可重新备份："
    echo "    $BACKUP_FILE"
    return 0
  fi
  step "备份原厂固件"
  info "正在读取 4MB Flash（约 15 秒，请勿拔线）…"
  "$ESPTOOL" --chip esp32c3 --port "$port" --baud 460800 \
            read_flash 0x0 0x400000 "$BACKUP_FILE" 2>&1 | tail -3
  if [ -f "$BACKUP_FILE" ]; then
    say "备份完成：$BACKUP_FILE"
  else
    err "备份失败！不要继续刷机。"
    exit 1
  fi
}

restore_factory() {
  ensure_esptool
  local port="$1"
  if [ ! -f "$BACKUP_FILE" ]; then
    err "没有找到备份文件：$BACKUP_FILE"
    exit 1
  fi
  warn "这会把你原来的闭源固件刷回去（短信转发功能恢复原样）"
  read -p "确定继续？(输入 y 确认) " a
  [ "$a" = "y" ] || { say "已取消"; return 0; }
  step "恢复原厂固件"
  "$ESPTOOL" --chip esp32c3 --port "$port" --baud 460800 \
            write_flash 0x0 "$BACKUP_FILE" 2>&1 | tail -5
  say "恢复完成，重新上电即可。"
}

# ---------------------------------------------------------------- 烧录固件
# 用 PlatformIO 烧录：bootloader + partitions + app + LittleFS 网页，一步到位
pio_flash() {
  local dir="$1" label="$2"
  step "烧录$label"
  ensure_pio
  if [ ! -d "$dir" ]; then
    err "找不到工程目录：$dir"
    exit 1
  fi
  info "目录：$dir"
  info "第一次编译需要几分钟，请耐心等待，不要拔线…"
  ( cd "$dir" && "$PIO_BIN" run -t upload )
  local rc=$?
  if [ $rc -ne 0 ]; then
    err "烧录失败（退出码 $rc）。常见原因："
    echo "  · USB 线不是数据线 / 一体机没上电"
    echo "  · 端口被别的程序占用（串口监视器、screen、Arduino IDE）"
    echo "  · Mac 弹了「允许访问配件」的提示没点允许"
    exit 1
  fi
  say "$label 烧录完成，设备正在重启"
}

# 监听串口，把设备 IP 抓出来
capture_ip() {
  local port="$1" seconds="${2:-35}"
  "$PY_BIN" - "$port" "$seconds" <<'PY' 2>/dev/null
import serial, sys, time, re
port, secs = sys.argv[1], float(sys.argv[2])
deadline = time.time() + secs
text = ""
try:
    s = serial.Serial(port, 115200, timeout=1)
except Exception:
    sys.exit(2)
while time.time() < deadline:
    try:
        chunk = s.read(s.in_waiting or 1)
    except Exception:
        break
    if chunk:
        text += chunk.decode("utf-8", "ignore")
s.close()
ips = re.findall(r"IP: (\d+\.\d+\.\d+\.\d+)", text)
if ips:
    print(ips[-1])
else:
    ips = re.findall(r"(\d+\.\d+\.\d+\.\d+)", text)
    print(ips[-1] if ips else "")
PY
}

# 等设备起来并做健康检查
wait_online() {
  local port="${1:-}"
  local ip=""
  if [ -n "$port" ]; then
    info "等待设备联网并打印 IP（最多 35 秒）…"
    ip="$(capture_ip "$(read_port "$port")" 35)"
  fi
  if [ -z "$ip" ] && [ -f "$STATE_FILE" ]; then
    ip="$(cat "$STATE_FILE")"
  fi
  if [ -z "$ip" ]; then
    warn "没能从串口读到 IP。可以手动输入设备 IP："
    read -p "IP 地址（如 192.168.5.171）：" ip
  fi
  [ -n "$ip" ] || { warn "跳过在线检查"; return 0; }
  echo "$ip" > "$STATE_FILE"

  step "健康检查"
  local code="000" body=""
  for i in 1 2 3 4 5 6 7 8 9 10; do
    code="$(curl -s -m 3 -o /tmp/.smsfw_health -w '%{http_code}' "http://$ip/api/health" 2>/dev/null)"
    [ "$code" = "200" ] && break
    sleep 2
  done
  if [ "$code" = "200" ]; then
    say "设备在线：http://$ip/"
    echo "    健康检查返回：$(cat /tmp/.smsfw_health 2>/dev/null)"
    echo
    info "下一步：浏览器打开上面的地址，用 admin / admin123 登录，"
    info "然后点「📶 扫描附近 WiFi」选信号最强的那个、填密码保存即可。"
    info "（如果已经填好 WiFi 并显示 IP，直接用就行）"
  else
    warn "没访问到 http://$ip/api/health（HTTP $code）"
    echo "  · 设备可能还在重启，稍等 10 秒后再运行： $0 状态"
    echo "  · 或确认 Mac 和设备连在同一个 WiFi 下"
  fi
}

flash_firmware() {
  check_device
  pio_flash "$FIRMWARE_DIR" "开源固件"
  wait_online "$PORT"
}

flash_scanner() {
  check_device
  pio_flash "$SCANNER_DIR" "引脚自检固件"
  warn "接下来：运行  $0 扫引脚  打开串口控制台（115200），输入 s 开始扫引脚"
}

scan_console() {
  check_device
  local tty
  tty="$(echo "$PORT" | sed 's|^/dev/cu\.|/dev/tty.|')"
  if ! command -v screen >/dev/null 2>&1; then
    err "系统缺少 screen 命令（brew install screen）"
    exit 1
  fi
  clear
  cat <<'EOF'

  ============================================================
    ESP32-C3 引脚自检  —  串口控制台
  ============================================================

    s          扫描接收引脚（扫描时给 SIM 发条短信！）
    f <rx>     搜索发送引脚
    e <rx>     搜索 EN/电源控制引脚
    m <rx> <tx> 实时监听模组串口
    t <rx> <tx> <文本>  发一条 AT 指令
    b <波特率>  切换波特率（默认 115200）
    q          退出

  --------------------------------------------------------
    按 Ctrl+A 然后按 K 可以退出 screen
  ============================================================

EOF
  screen "/dev/tty.${PORT#/dev/cu.}" 115200
}

status() {
  step "设备状态"
  local ip=""
  if [ -f "$STATE_FILE" ]; then
    ip="$(cat "$STATE_FILE")"
    info "上次记录的 IP：$ip"
  fi
  read -p "设备 IP（直接回车用上面记录的）：" ans
  [ -n "$ans" ] && ip="$ans"
  if [ -z "$ip" ]; then
    warn "没有 IP 可以检查"
    return 0
  fi
  echo "$ip" > "$STATE_FILE"
  local code="$(curl -s -m 5 -o /tmp/.smsfw_health -w '%{http_code}' "http://$ip/api/health" 2>/dev/null)"
  if [ "$code" = "200" ]; then
    say "设备在线：http://$ip/"
    echo "    健康检查返回：$(cat /tmp/.smsfw_health 2>/dev/null)"
    curl -s -m 5 -u admin:admin123 "http://$ip/api/wifi" 2>/dev/null | head -c 400
    echo
  else
    warn "http://$ip/api/health 返回 HTTP $code（设备可能离线）"
  fi
}

open_web() {
  local ip=""
  [ -f "$STATE_FILE" ] && ip="$(cat "$STATE_FILE")"
  if [ -z "$ip" ]; then
    read -p "设备 IP： " ip
  fi
  [ -n "$ip" ] || return 0
  say "正在打开浏览器：http://$ip/"
  open "http://$ip/" 2>/dev/null || echo "请手动在浏览器打开：http://$ip/"
}

menu() {
  while true; do
    clear
    cat <<'EOF'

  ============================================================
      短信转发器 一键刷机助手
      ESP32-C3 + ML307A 一体机
  ============================================================

    [1] 检查设备连接状态
    [2] 备份原厂固件（强烈建议先做一次）
    [3] 烧录自研开源固件（推荐，网页在 data/index.html）
    [4] 烧录引脚自检固件（排查接线用）
    [5] 打开扫引脚控制台
    [6] 恢复原厂固件
    [7] 查看设备 IP / 健康检查
    [8] 打开管理网页（浏览器）

    [0] 退出

  ============================================================

EOF
    read -p "请选择：" c
    case "$c" in
      1) check_device ;;
      2) check_device; backup "$PORT" ;;
      3) flash_firmware ;;
      4) flash_scanner ;;
      5) scan_console ;;
      6) check_device; restore_factory "$PORT" ;;
      7) status ;;
      8) open_web ;;
      0) exit 0 ;;
      *) warn "无效选项" ; sleep 1 ;;
    esac
    echo
    read -p "按回车返回菜单…" _
  done
}

case "${1:-}" in
  刷固件)   flash_firmware ;;
  备份)     check_device; backup "$PORT" ;;
  恢复原厂) check_device; restore_factory "$PORT" ;;
  刷扫描器) flash_scanner ;;
  扫引脚)   scan_console ;;
  状态)     status ;;
  网页)     open_web ;;
  *)        menu ;;
esac
