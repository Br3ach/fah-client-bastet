# CPU affinity scheduling regressions

Run `python tests/cpu-affinity-scheduling/run.py` from the client repository root. On Windows use a Visual Studio Developer terminal; on other platforms provide a C++17 compiler through CXX if necessary.

The runner extracts the current Unit::blocksCPULaunch implementation and GPU scheduling loops from Group.cpp, compiles them with lightweight Unit/process adapters, and verifies retained live-mask reservations, mode transitions, process release, shared GPU helpers, multiple GPUs, zero CPU budgets and unchanged CPU-folding budgets. It does not launch real FahCore processes or prove OS affinity enforcement.

Only CPU-folding WUs consume managed RG masks. GPU WUs receive their minimum helper count (at least one), use shared OS scheduling and never require the user to reserve a helper CPU. Helpers can contend with CPU folding, as normal GPU folding does; the CPU count is not a promise of unused CPUs for GPU support. Legacy count-only scheduling remains unchanged.

New CPU-WU launches check every running CPU WU across all groups. A process retains its running mask until finalizeRun releases its process reference. A conflicting launch waits and retries; desired masks may change without releasing the running reservation. When either CPU WU is unpinned during a managed/legacy transition, the check conservatively waits for exit.

Pin validation cases also check raw logical capacities of 8 and 16, rejection of excessive API counts, unsupported pinning, and preserved saved intent. For actual observable/database/notification integration run: python tests/api-configuration-smoke.py --client /path/to/fah-client.exe. On Windows put the matching OpenSSL DLLs beside the executable or on PATH. The unsupported-pin scenario requires a machine without reported performance classes; all data is temporary and paused. It does not exercise real FahCore processes.

The restart regression executes the production running-process monitor block and affinity calculation. It checks GPU helper bookkeeping changes, pin and topology changes, CPU count and mask changes, pause/state transitions, and shutdown/startup grace behavior.

The configuration-update harness checks that group-only saves reconcile once, changed global settings still reconcile, and rejected group saves preserve global settings.
