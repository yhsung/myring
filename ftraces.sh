# save as filter_ftrace_events.sh ; chmod +x filter_ftrace_events.sh
#!/usr/bin/env bash
CANDIDATES=(
  "kmem/kmalloc" "kmem/kmalloc_node" "kmem/kfree"
  "mm_page_alloc/mm_page_alloc" "mm_page_alloc/mm_page_free"
  "vmscan/mm_vmscan_direct_reclaim_begin" "vmscan/mm_vmscan_direct_reclaim_end"
  "vmscan/mm_vmscan_kswapd_wake" "vmscan/mm_vmscan_kswapd_sleep"
  "writeback/writeback_start" "writeback/writeback_written"
  "oom/oom_kill" "psi/psi_event"
)
OK=()
for ev in "${CANDIDATES[@]}"; do
  if [ -d "/sys/kernel/tracing/events/$ev" ]; then
    OK+=("$ev")
  fi
done
printf "%s\n" "${OK[@]}"

