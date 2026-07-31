# Logging and crash diagnostics

The HTTP service uses spdlog for console, rotating-file, and request logs.
Default file: `logs/lw-ppocr.log`.

With `logging.enabled=true`, `logging.level=info`, and
`logging.request_enabled=true`, every OCR request writes two records:

1. `request_started` before JSON/Base64 decoding and inference begins.
2. A completion record with status, elapsed time, and result count.

The start record is flushed immediately. If the process exits during a request,
its request ID, source address, path, and body size should therefore remain in
the log. Caught inference exceptions, `std::terminate`, and Windows unhandled
exception code/address are also logged and flushed on a best-effort basis.

For privacy, logs never contain API Keys, request bodies, Base64 image data, or
recognized text.

## Limits

A log is not a crash dump. It cannot reliably provide a native stack trace, and
no process can guarantee a final log write after power loss, `kill -9`, severe
memory corruption, disk failure, or some operating-system terminations.

For production diagnostics, keep the rotating log and also enable:

- Windows Error Reporting or ProcDump minidumps on Windows.
- `systemd` journal plus `coredumpctl`/core dumps on Linux.
- Service-manager restart policy and external health monitoring.

Use the final `request_started` entry to correlate a dump or system event with
the request that was active when the process stopped.
