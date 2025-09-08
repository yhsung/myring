#!/bin/bash
# 控制 Perfetto traced / traced_probes 的常駐服務
# 用法:
#   ./perfetto_service.sh start
#   ./perfetto_service.sh stop
#   ./perfetto_service.sh status

BIN_DIR=/mnt/hostshare/perfetto/out/linux_gcc_release_arm64   # 修改成你 build 出來的路徑

start_services() {
  pkill -9 traced || true
  pkill -9 traced_probes || true
  sleep 1
  echo "[*] 啟動 traced ..."
  $BIN_DIR/tracebox traced &
  echo "[*] 啟動 traced_probes ..."
  $BIN_DIR/tracebox traced_probes &
  echo "[*] Perfetto services 已啟動"
  $BIN_DIR/tracebox websocket_bridge &
  echo "[*] Websocket services 已啟動"
}

stop_services() {
  pkill -9 traced || true
  pkill -9 traced_probes || true
  echo "[*] Perfetto services 已停止"
}

status_services() {
  pgrep -a traced || echo "traced 沒有執行"
  pgrep -a traced_probes || echo "traced_probes 沒有執行"
}

case "$1" in
  start) start_services ;;
  stop) stop_services ;;
  status) status_services ;;
  *) echo "用法: $0 {start|stop|status}" ;;
esac
