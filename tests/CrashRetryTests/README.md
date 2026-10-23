Build the client with the crash-retry change, then build this executable with
the existing `tests/SConstruct` (the client library must contain the change).
On Windows, run:

```
python tests/CrashRetryTests/run.py tests/CrashRetryTests/crashRetry.exe
```

The runner uses a temporary working directory and two separate processes to
verify SQLite persistence through Unit::next() and restoration of the
one-attempt budget. It also checks recovery with an exhausted ordinary retry
budget and the independent two-second crash delay.
It does not dispatch the client's event loop or contact assignment servers.
Children return controlled Windows exception exit codes through the actual
CoreProcess/Unit exit path. These tests do not reproduce a CUDA fault or verify
restoration of a science core's checkpoint. Unix core-dump handling needs Unix
validation; the native exception-exit cases here run on Windows.
