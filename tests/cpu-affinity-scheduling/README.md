# CPU affinity scheduling regressions

Run `python tests/cpu-affinity-scheduling/run.py` from the client repository root. On Windows use a Visual Studio Developer terminal; on other platforms provide a C++17 compiler through CXX if necessary.

The runner extracts the current Unit::blocksCPULaunch implementation and GPU scheduling loops from Group.cpp, compiles them with lightweight Unit/process adapters, and verifies retained live-mask reservations, mode transitions, process release, shared GPU helpers, multiple GPUs, zero CPU budgets and unchanged CPU-folding budgets. It does not launch real FahCore processes or prove OS affinity enforcement.

CPU WUs consume managed RG masks. GPU WUs receive their minimum helper bookkeeping count (at least one). The per-RG `gpu_reserved_cores` setting defaults to zero: GPU FahCores use hard affinity to the shared, unreserved Performance 1 logical CPUs when supported. Positive values reserve complete physical Performance 1 cores, including all SMT siblings, before CPU allocation in every RG. Each enabled GPU receives a separate whole-core reservation. Disabled GPU selections make the saved reservation inactive. An unavailable exclusive allocation waits rather than launching unrestricted.

A positive reservation also enables globally managed CPU allocation in count-only hybrid systems. Count-only scheduling otherwise retains its existing behaviour. Configuration edits validate CPU class/total capacity after subtracting GPU reservations; unrelated edits preserve saved intent during topology loss.

New exclusive CPU or GPU launches check conflicting running processes across all groups. Shared GPU helpers may overlap CPU work, and each enabled GPU receives its own reserved mask. A process retains its running mask until finalizeRun releases its process reference. A conflicting launch waits and retries; desired masks may change without releasing the running reservation. When either CPU WU is unpinned during a managed/legacy transition, the check conservatively waits for exit.

For observable/database/notification integration run: `python tests/api-configuration-smoke.py --client /path/to/fah-client.exe`. On Windows put the matching OpenSSL DLLs beside the executable or on PATH. All data is temporary and paused. It does not exercise real FahCore processes.

The restart regression executes the production monitor, desired-affinity calculation and launch-exclusion functions. It checks stable reserved GPU launches, helper bookkeeping changes, reservation enable/disable, mask changes, separate same-RG GPU reservations, cross-RG exclusion, CPU pool/count changes, pause/state transitions and shutdown/startup grace. `gpu-validation.py` exercises the production prospective-capacity and strict numeric API rejection blocks. These adapters do not replace a full linked client or Windows integration test.

The configuration-update harness checks that group-only saves reconcile once, changed global settings still reconcile, and rejected group saves preserve global settings.
