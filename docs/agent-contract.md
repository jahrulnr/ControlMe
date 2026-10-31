# Agent contract tool

Call the MCP tool `contract` with `{}` to read the core operating guidance.
Call it with `{"reference":"camera"}`, `{"reference":"motion"}`,
`{"reference":"storage"}`, `{"reference":"device"}`, or
`{"reference":"camera-format"}` to read a focused
reference. The MCP text result contains a JSON object with a `content` string.
Unknown references return `NOT_FOUND`; invalid names return `INVALID_ARGUMENT`.

The source documents are in `contracts/`. PlatformIO embeds them in the
firmware; on startup the device installs changed documents to `/contracts/`
on LittleFS before starting the log owner. Unchanged files are left alone.
Changes to bundled documents take effect after building and uploading firmware
and restarting the device. No filesystem image upload is needed. If mounting
or installation fails, `contract` is not advertised in `tools/list` and the
startup log records an installation failure when possible.

To add a bundled reference, add its Markdown file to `contracts/`, list it in
`board_build.embed_txtfiles` in `platformio.ini`, add its embedded symbol and
name to `src/contract_store.cpp`, and link it from `contracts/core.md`.
Reference names use lowercase letters, digits, `-`, or `_` (maximum 48
characters). Each file must fit within the 3072-byte read limit and the
6143-byte JSON-escaped result limit. Files installed separately in the same
directory are also readable by valid reference name; bundled names are restored
to their firmware contents on startup.

Run `bash test/run_contract_tests.sh` for storage behavior against a fake
LittleFS, `bash test/run_host_tests.sh` for the pure protocol suites, and
`pio run -e esp32cam` for the firmware. After an authorized firmware upload,
verify `tools/list`, `contract` with `{}` and each bundled reference, plus an
unknown reference, on the device.
