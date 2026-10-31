# SD storage

Check `device_status.sdMounted` before using `sd_*`. Paths refer to the SD card, not LittleFS. Use `sd_list` with `path`, following `nextCursor` while results are truncated. `sd_stat` returns size and revision. `sd_read` returns base64 data with `nextOffset` and `eof`; continue from the returned offset.

For `sd_write`, call `BEGIN` with a path and `mode` (`create` or `replace`); replacement requires `expectedRevision` from `sd_stat`. Send `CHUNK` calls using the returned `uploadId`, `offset`, and `dataBase64`, advancing to each returned `nextOffset`. Finish with `COMMIT` and the exact `totalBytes`; use `ABORT` if abandoning the upload. A timeout does not prove a write stopped: inspect the result/state before retrying. Use `sd_rename` and `sd_delete` only for the intended paths.

Camera staging is separate from SD; `camera_capture` does not save a file automatically.
