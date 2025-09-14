#!/bin/bash
# =============================================================================
# Perfetto Service Management Script
# =============================================================================
# 
# This script manages Perfetto tracing daemon services for system performance
# analysis and debugging. It supports both manual process management and
# systemd integration for production deployments.
#
# Services managed:
# - traced:           Main tracing service daemon
# - traced_probes:    Data source probes daemon  
# - websocket_bridge: Remote access interface
#
# Usage:
#   ./perfetto-service.sh start     # Start all Perfetto services
#   ./perfetto-service.sh stop      # Stop all Perfetto services
#   ./perfetto-service.sh status    # Check service status
#   ./perfetto-service.sh systemd   # Generate systemd service files
#
# SystemD Integration:
#   ./perfetto-service.sh systemd   # Create service unit files
#   sudo systemctl daemon-reload    # Reload systemd configuration
#   sudo systemctl enable perfetto-traced perfetto-traced-probes perfetto-websocket
#   sudo systemctl start perfetto-traced perfetto-traced-probes perfetto-websocket
#
# Requirements:
# - Perfetto binaries built for target architecture
# - Appropriate permissions for process management
# - Network access for websocket bridge (if used remotely)
# - systemd (for systemd integration)
#
# =============================================================================
# Configuration
# =============================================================================

# Path to Perfetto binaries - modify this to match your build output
BIN_DIR=/mnt/hostshare/perfetto/out/linux_gcc_release_arm64

# SystemD service files directory
SYSTEMD_DIR=/etc/systemd/system

# Service user (for systemd services)
SERVICE_USER="perfetto"

# Detect if systemd is available
HAS_SYSTEMD=false
if command -v systemctl >/dev/null 2>&1; then
    HAS_SYSTEMD=true
fi

# =============================================================================
# Service Management Functions
# =============================================================================

start_services() {
  echo "[INFO] Starting Perfetto services..."
  
  # Check if systemd is available and services are installed
  if [ "$HAS_SYSTEMD" = true ] && systemctl list-unit-files | grep -q "perfetto-traced.service"; then
    echo "[INFO] Using systemd to start services..."
    
    sudo systemctl start perfetto-traced.service
    sudo systemctl start perfetto-traced-probes.service
    sudo systemctl start perfetto-websocket.service
    
    echo "[SUCCESS] Perfetto systemd services started"
    return
  fi
  
  # Fallback to manual process management
  echo "[INFO] Using manual process management..."
  
  # Clean shutdown of any existing services
  echo "[INFO] Stopping any existing Perfetto processes..."
  pkill -9 traced || true
  pkill -9 traced_probes || true
  pkill -9 websocket_bridge || true
  
  # Brief pause to ensure clean shutdown
  sleep 1
  
  # Start core tracing daemon
  echo "[INFO] Starting traced daemon..."
  $BIN_DIR/tracebox traced &
  TRACED_PID=$!
  
  # Start data source probes
  echo "[INFO] Starting traced_probes daemon..."
  $BIN_DIR/tracebox traced_probes &
  PROBES_PID=$!
  
  # Start websocket bridge for remote access
  echo "[INFO] Starting websocket bridge..."
  $BIN_DIR/tracebox websocket_bridge &
  BRIDGE_PID=$!
  
  echo "[SUCCESS] Perfetto services started successfully"
  echo "  - traced PID: $TRACED_PID"
  echo "  - traced_probes PID: $PROBES_PID"
  echo "  - websocket_bridge PID: $BRIDGE_PID"
}

stop_services() {
  echo "[INFO] Stopping Perfetto services..."
  
  # Check if systemd is available and services are installed
  if [ "$HAS_SYSTEMD" = true ] && systemctl list-unit-files | grep -q "perfetto-traced.service"; then
    echo "[INFO] Using systemd to stop services..."
    
    sudo systemctl stop perfetto-traced.service || true
    sudo systemctl stop perfetto-traced-probes.service || true
    sudo systemctl stop perfetto-websocket.service || true
    
    echo "[SUCCESS] Perfetto systemd services stopped"
    return
  fi
  
  # Fallback to manual process termination
  echo "[INFO] Using manual process termination..."
  
  # Gracefully terminate processes first
  echo "[INFO] Sending SIGTERM to services..."
  pkill -TERM traced 2>/dev/null || true
  pkill -TERM traced_probes 2>/dev/null || true
  pkill -TERM websocket_bridge 2>/dev/null || true
  
  # Wait for graceful shutdown
  sleep 2
  
  # Force termination if still running
  echo "[INFO] Force terminating remaining processes..."
  pkill -9 traced || true
  pkill -9 traced_probes || true
  pkill -9 websocket_bridge || true
  
  echo "[SUCCESS] All Perfetto services stopped"
}

status_services() {
  echo "[INFO] Checking Perfetto service status..."
  echo ""
  
  # Check if systemd services are available
  if [ "$HAS_SYSTEMD" = true ] && systemctl list-unit-files | grep -q "perfetto-traced.service"; then
    echo "=== SystemD Service Status ==="
    echo "traced service:"
    systemctl is-active perfetto-traced.service 2>/dev/null || echo "inactive"
    echo "traced_probes service:"
    systemctl is-active perfetto-traced-probes.service 2>/dev/null || echo "inactive"
    echo "websocket bridge service:"
    systemctl is-active perfetto-websocket.service 2>/dev/null || echo "inactive"
    echo ""
  fi
  
  # Check running processes
  echo "=== Process Status ==="
  
  # Check traced daemon
  echo "traced daemon:"
  if pgrep -a traced >/dev/null 2>&1; then
    pgrep -a traced
    echo "Status: RUNNING"
  else
    echo "Status: NOT RUNNING"
  fi
  echo ""
  
  # Check traced_probes daemon
  echo "traced_probes daemon:"
  if pgrep -a traced_probes >/dev/null 2>&1; then
    pgrep -a traced_probes
    echo "Status: RUNNING"
  else
    echo "Status: NOT RUNNING"
  fi
  echo ""
  
  # Check websocket bridge
  echo "websocket_bridge:"
  if pgrep -a websocket_bridge >/dev/null 2>&1; then
    pgrep -a websocket_bridge
    echo "Status: RUNNING"
  else
    echo "Status: NOT RUNNING"
  fi
  echo ""
}

# =============================================================================
# Main execution logic
# =============================================================================

generate_systemd_services() {
  echo "[INFO] Generating systemd service files..."
  
  # Create perfetto-traced.service
  cat > /tmp/perfetto-traced.service << EOF
[Unit]
Description=Perfetto traced daemon
Documentation=https://perfetto.dev/
After=network.target
Wants=network.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart=$BIN_DIR/tracebox traced
Restart=always
RestartSec=10
KillMode=process
TimeoutStopSec=30
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=/tmp /var/tmp
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
EOF

  # Create perfetto-traced-probes.service
  cat > /tmp/perfetto-traced-probes.service << EOF
[Unit]
Description=Perfetto traced probes daemon
Documentation=https://perfetto.dev/
After=perfetto-traced.service
Requires=perfetto-traced.service

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart=$BIN_DIR/tracebox traced_probes
Restart=always
RestartSec=10
KillMode=process
TimeoutStopSec=30
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=/tmp /var/tmp
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
EOF

  # Create perfetto-websocket.service
  cat > /tmp/perfetto-websocket.service << EOF
[Unit]
Description=Perfetto websocket bridge
Documentation=https://perfetto.dev/
After=perfetto-traced.service perfetto-traced-probes.service
Requires=perfetto-traced.service
Wants=perfetto-traced-probes.service

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart=$BIN_DIR/tracebox websocket_bridge
Restart=always
RestartSec=10
KillMode=process
TimeoutStopSec=30
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=/tmp /var/tmp
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
EOF

  echo "[SUCCESS] SystemD service files generated in /tmp/"
  echo ""
  echo "To install:"
  echo "  sudo cp /tmp/perfetto-*.service $SYSTEMD_DIR/"
  echo "  sudo systemctl daemon-reload"
  echo "  sudo systemctl enable perfetto-traced perfetto-traced-probes perfetto-websocket"
  echo "  sudo systemctl start perfetto-traced perfetto-traced-probes perfetto-websocket"
  echo ""
  echo "To check status:"
  echo "  systemctl status perfetto-traced perfetto-traced-probes perfetto-websocket"
}

case "${1:-}" in
  start)
    start_services
    ;;
  stop)
    stop_services
    ;;
  status)
    status_services
    ;;
  restart)
    echo "[INFO] Restarting Perfetto services..."
    stop_services
    sleep 2
    start_services
    ;;
  systemd)
    generate_systemd_services
    ;;
  *)
    echo "Usage: $0 {start|stop|status|restart|systemd}"
    echo ""
    echo "Commands:"
    echo "  start   - Start all Perfetto services"
    echo "  stop    - Stop all Perfetto services"
    echo "  status  - Check service status"
    echo "  restart - Stop and start services"
    echo "  systemd - Generate systemd service files"
    echo ""
    echo "Examples:"
    echo "  $0 start    # Start tracing services"
    echo "  $0 status   # Check if services are running"
    echo "  $0 restart  # Restart all services"
    echo "  $0 systemd  # Generate systemd service files"
    echo ""
    echo "SystemD Integration:"
    echo "  1. $0 systemd                    # Generate service files"
    echo "  2. sudo cp /tmp/perfetto-*.service /etc/systemd/system/"
    echo "  3. sudo systemctl daemon-reload  # Reload systemd"
    echo "  4. sudo systemctl enable perfetto-traced perfetto-traced-probes perfetto-websocket"
    echo "  5. sudo systemctl start perfetto-traced perfetto-traced-probes perfetto-websocket"
    exit 1
    ;;
esac
