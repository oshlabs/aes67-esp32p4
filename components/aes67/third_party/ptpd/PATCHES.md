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
- `ptp_net_recv()`: strips the Ethernet header from the returned length and
  clamps the copy to the caller's buffer; frames longer than the receive
  buffer (TLVs) used to overflow `rxbuf` into the two-step state. `rxbuf.raw`
  grows to 256 bytes.
- `is_better_clock()`: compares the announce data set lexicographically
  (priority1, class, accuracy, variance, priority2, identity) instead of
  returning true when any single field is lower.
- `ptp_lock_local_clock_freq()`: the path-delay stability gate compares
  against the previous sample (it compared the offset with itself), the
  counter lives in the state and is reset after a clock step.
- UDP transport quality: `ptpd_hook_input()` lets the Ethernet input hook hand
  PTP/UDP multicast frames to the daemon with the EMAC's receive timestamp;
  the daemon loop waits on that queue. Delay requests go out on the transport
  the selected master uses, as a raw frame through the driver when that is
  UDP, so the hardware transmit stamp belongs to the frame the master sees.
  Sync/follow-up are sent per transport with each transport's own stamp.
  `IP_MULTICAST_LOOP` is off on the transmit socket. The daemon task runs at
  `CONFIG_NETUTILS_PTPD_SERVERPRIO` instead of idle+2.
- `ptpd_configure(domain, allow_server)`: the domain and the permission to act
  as master are runtime settings (from `aes67_ptp_config_t`), with the
  Kconfig values as defaults; the three `CONFIG_NETUTILS_PTPD_DOMAIN` uses and
  the server branch of `ptp_periodic_send()` read them.
