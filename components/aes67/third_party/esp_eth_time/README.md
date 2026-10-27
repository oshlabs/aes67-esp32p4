# ESP Ethernet Time Control Component Example

This example component provides a wrapper around management of the internal Ethernet MAC Time (Time Stamping) system which is normally accessed via `esp_eth_ioctl` commands. The component is offering a more intuitive API mimicking POSIX `clock_settime`, `clock_gettime` group of time functions and so making it easier to integrate with existing systems.

## Local changes

- `esp_eth_clock_gettime()` re-reads the MAC time when the first read lands
  in the first millisecond of a second. The MAC exposes seconds and
  sub-seconds as two registers; a rollover between the two reads yields the
  old second with the new nanoseconds, one second early. The frame timer,
  the frame task and the audio clock loop all read this clock from
  interrupt or task context and would see a one-second step.
