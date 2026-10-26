# Local changes to the vendored PTP daemon

The daemon is the Apache NuttX `ptpd` as ported to ESP-IDF by Espressif in the
`ethernet/ptp` example. Changes made in this repository, so the copy can be
re-synced against a newer upstream:

- `ptp_destroy_state()` (ESP_PTP path): also close the three UDP sockets the
  ESP path creates in `ptp_initialize_state()`; they leaked on every stop.
- `ptpd_stop()` (ESP_PTP path): return `-ESRCH` when no daemon runs, and wait
  for the daemon task to signal its exit (`s_daemon_done`) so a following
  `ptpd_start()` does not race the old task for `s_state` and the sockets.
- `ptpd_start()` (ESP_PTP path): returns `-EBUSY` if a daemon runs, and waits
  for the daemon task to report the result of `ptp_initialize_state()`, so a
  failure (unknown interface key, tap not registered) comes back as a negative
  errno instead of a NULL dereference in a later `ptpd_status()`.
- `ptpd_status()`: returns `-ESRCH` when no daemon runs; the status-request
  handoff is done under a spinlock and the daemon works on a local copy.
