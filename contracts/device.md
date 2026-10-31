# Device operations and errors

`device_status` reports heap, Wi-Fi, uptime, SD mount, and wall-clock state. `device_set_time` accepts Unix epoch milliseconds; a later SNTP synchronization can override it. `log_read` selects `current` or `previous`, with `offset` and `length`; use `nextOffset` to paginate or `downloadUrl` for the complete log.

Inspect the tool error code and detail. `NOT_FOUND` can mean an expired or replaced staged image; capture again if needed. `RESOURCE_BUSY` can mean a pinned image pool, active camera stream, or a servo/SD handoff. `SD_UNAVAILABLE` means SD operations cannot currently proceed. A `TIMEOUT` means the caller stopped waiting, not necessarily that the operation stopped: inspect state before repeating a mutation.

`device_reboot` accepts an optional reason and returns `ACCEPTED` before the reboot completes. Reconnect and check status afterward. Staged image IDs do not survive reboot.
