#!/bin/bash
# =============================================================================
# Perfetto Service Management Script
# =============================================================================
# 
# This script manages Perfetto tracing daemon services for system performance
# analysis and debugging. It controls the lifecycle of traced and traced_probes
# services required for Perfetto trace collection.
#
# Services managed:
# - traced:        Main tracing service daemon
# - traced_probes: Data source probes daemon  
# - websocket_bridge: Remote access interface
#
# Usage:
#   ./perfetto-service.sh start   # Start all Perfetto services
#   ./perfetto-service.sh stop    # Stop all Perfetto services
#   ./perfetto-service.sh status  # Check service status
#
# Requirements:
# - Perfetto binaries built for target architecture
# - Appropriate permissions for process management
# - Network access for websocket bridge (if used remotely)
#
# =============================================================================
# Configuration
# =============================================================================

# Path to Perfetto binaries - modify this to match your build output
BIN_DIR=/mnt/hostshare/perfetto/out/linux_gcc_release_arm64

# =============================================================================
# Service Management Functions
# =============================================================================

start_services() {
  echo "[INFO] Starting Perfetto services..."
  
  # Clean shutdown of any existing services
  echo "[INFO] Stopping any existing Perfetto processes..."
  pkill -9 traced || true
  pkill -9 traced_probes || true
  
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
  
  # Forcefully terminate all Perfetto processes
  echo "[INFO] Terminating traced daemon..."
  pkill -9 traced || true
  
  echo "[INFO] Terminating traced_probes daemon..."
  pkill -9 traced_probes || true
  
  echo "[INFO] Terminating websocket bridge..."
  pkill -9 websocket_bridge || true
  
  echo "[SUCCESS] All Perfetto services stopped"
}

status_services() {
  echo "[INFO] Checking Perfetto service status..."
  echo ""
  
  # Check traced daemon
  echo "=== traced daemon ==="
  if pgrep -a traced >/dev/null 2>&1; then
    pgrep -a traced
    echo "Status: RUNNING"
  else
    echo "Status: NOT RUNNING"
  fi
  echo ""
  
  # Check traced_probes daemon
  echo "=== traced_probes daemon ==="
  if pgrep -a traced_probes >/dev/null 2>&1; then
    pgrep -a traced_probes
    echo "Status: RUNNING"
  else
    echo "Status: NOT RUNNING"
  fi
  echo ""
  
  # Check websocket bridge
  echo "=== websocket_bridge ==="
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
  *)
    echo "Usage: $0 {start|stop|status|restart}"
    echo ""
    echo "Commands:"
    echo "  start   - Start all Perfetto services"
    echo "  stop    - Stop all Perfetto services"
    echo "  status  - Check service status"
    echo "  restart - Stop and start services"
    echo ""
    echo "Examples:"
    echo "  $0 start    # Start tracing services"
    echo "  $0 status   # Check if services are running"
    echo "  $0 restart  # Restart all services"
    exit 1
    ;;
esac
