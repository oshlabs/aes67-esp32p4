# Local changes to the vendored PTP daemon

The daemon is the Apache NuttX `ptpd` as ported to ESP-IDF by Espressif in the
`ethernet/ptp` example. Changes made in this repository, so the copy can be
re-synced against a newer upstream:

- `ptp_destroy_state()` (ESP_PTP path): also close the three UDP sockets the
  ESP path creates in `ptp_initialize_state()`; they leaked on every stop.
- `ptpd_stop()` (ESP_PTP path): return `-ESRCH` when no daemon runs, and wait
  for the daemon task to signal its exit (`s_daemon_done`) so a following
  `ptpd_start()` does not race the old task for `s_state` and the sockets.
