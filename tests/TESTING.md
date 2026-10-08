# Possible Testing Directions

See ARCHITECTURE.md.

## External dependencies (test isolation points)

Anything we want to test in isolation has to be replaceable here:

| Dependency | Where it lives | Effect |
|---|---|---|
| Assignment Servers | `assignment-servers` option | HTTP POSTs from `Unit::assign()` |
| Work Servers | URL in AS response | `Unit::download()`, `Unit::upload()` |
| Collection Servers | URL list in WU data | Fallback upload destinations |
| `api.foldingathome.org` | `api-server` option | `Account` WS connection |
| Core URLs (download) | URL in AS response | `Core::download()` |
| FahCore binaries (exec) | `cores/...` | `CoreProcess::exec()` |
| Filesystem | `work/<id>/`, `cores/`, `credits/`, `client.db` | WU data, results, credits, persistence |
| Time | `cb::Time::now()` | Backoff, deadlines, clock skew |
| System info | `cb::SystemInfo` | CPU count, machine ID, memory |
| GPU detection | `cb::GPUInfo` / `cb::GPUDevice` | `GPUResources::detect()` |
| Power management | `cb::PowerManagement` | Battery, idle, keep-awake |
| Signals | `SIGINT`, `SIGTERM`, `SIGHUP` | Shutdown |

## Wire-protocol artifacts

These are part of the public API between client, AS, WS, and frontend.
Changes here break compatibility:

- `UnitState` enum names (`UNIT_ASSIGN`, `UNIT_DOWNLOAD`, `UNIT_CORE`,
  `UNIT_RUN`, `UNIT_UPLOAD`, `UNIT_DUMP`, `UNIT_DONE`) — JSON values.
- `CoreState` enum names.
- `ExitCode` enum values (numeric, set by science core binary).
- AS request/response JSON shape — see `Unit::writeRequest`,
  `Unit::assignResponse`.
- WS download/upload/dump JSON shape — see `Unit::downloadResponse`,
  `Unit::upload`, `Unit::dump`.
- The observable JSON tree the frontend sees — see `App::loadConfig`
  for the `info` shape and the default group JSON
  (`src/resources/group.json`) for `config`.

## Critical functionality worth testing

In approximate priority order:

1. **Unit state machine progression** — happy path
   `ASSIGN → DOWNLOAD → CORE → RUN → UPLOAD → DONE`.
2. **Retry / backoff** — failure responses trigger correct delay,
   correct retry-cap behavior, switch to CS list on upload failure.
3. **Dump path** — invalid response, missing results, expired WU, user
   "dump" command from all reachable states.
4. **Pause / resume** — during each state, scheduler-driven pause vs.
   user-driven pause, finish-on-complete semantics.
5. **Group scheduler resource allocation** — CPU/GPU partition under
   competing units, max-WU cap, finish mode, GPU minimum-CPU requirement.
6. **Persistence** — unit serialized to DB across each transition;
   restart loads units and resumes correctly; `UNIT_RUN` is reset to
   `UNIT_CORE` on load.
7. **Core process exit-code handling** — every `ExitCode` mapped to the
   correct next state in `finalizeRun()`.
8. **Signature verification** — AS/WS response with bad cert / bad
   signature / wrong key-usage is rejected (negative tests).
9. **Clock skew detection** — large `Time::now()` jump during run gets
   detected and time estimates adjusted.
10. **Account state machine** — link/connect/retry with backoff.
11. **Config merge** — defaults + DB + options + remote updates.

## Where to look next

- `Unit::next()` in `Unit.cpp` — `next()`, the state-machine dispatcher.
- `Group::update()` in `Group.cpp` — `update()`, the resource scheduler.
- `App::init()` / `App::run()` in `App.cpp` — `init()`/`run()`, the startup path.
- `src/resources/group.json` — default config shape (the JSON the
  frontend sees).
- `Account.cpp` — the account-bridge state machine, the part of the
  client most likely to surprise you.

## CPU affinity regression targets

From the repository root, build ordinary tests with `scons -C tests`, then run
`python "$CBANG_HOME/tests/testHarness" -C tests --no-color` (on Windows use
`%CBANG_HOME%` instead). Avoid `scons -C tests test`: cbang intercepts that target
and tries to enter `tests/tests`. Run scheduling, SQLite transaction and synthetic
allocator suites with `scons -C tests affinity-fast`, and real OS strict-launch
checks with `scons -C tests affinity-integration`. Use a configured compiler
environment with CBANG_HOME set. The obsolete adaptive-worker suite has been removed.

Execution-plan and allocator tests link the shipped CPUExecutionPlan.cpp.
The API smoke test uses the real client executable, a temporary database and
paused groups with no work units. Service endpoints point to loopback instead of
FAH servers. Run `python tests/api-configuration-smoke.py --client PATH_TO_CLIENT`.
It checks malformed request rejection against complete group configuration,
published allocation metadata and persisted group rows, then verifies a valid API
edit and restart persistence. It also checks legacy pin_to_perf_cores removal.
Both Linux and Windows CI execute it. This does not test live WU scheduling or
inject database failures into the real client.


### Affinity coverage and hardware checks

The execution-policy test exhausts 151,104 small partition cases with three
competing groups, core widths 1/2/3/6, rotated physical-core order, an excluded
whole core, and both spare-pool modes. It checks available-CPU membership,
disjoint logical ownership, whole-core atomicity, zero-demand groups, worker
budget bounded by pool capacity, and a8/a9 versus ordinary process masks.
It intentionally does not require globally optimal capacity when bounded repair/search declines a case. The suite checks successful rebalancing, exhaustive infeasibility, the 128-core skip and the 50,000-state exhaustion; the core limit preserves original pools, while state exhaustion retains the best safe packing found and never reduces fulfilled workers. An independent three-group oracle covers 2,250 small partial-packing cases, including infeasible requests. Planner/publication tests verify the diagnostic reports remain deterministic and swap atomically with the result.

`strict-affinity/linux-native.py` also changes the mask of its own isolated
Linux child, checks the live mask, waits for termination, and verifies an exact
relaunch. The live shrink check explicitly skips on a single-CPU environment.
It does not change host cgroups or physical topology, and is not a replacement
for hot-plug integration. Scheduling/allocator topology fixtures remain synthetic.

Before release, record OS, CPU model, topology, masks and logs for these real
hardware cases on both a hybrid machine and a homogeneous SMT machine:

- Enable a GPU reservation, then change its size while a GPU WU is running.
  Confirm graceful restart and that conflicting CPU/GPU launches wait until
  the old live process exits. CPU work must exclude every reserved sibling.
- Make a saved positive reservation unavailable in a disposable test setup.
  Confirm an empty desired mask, a published GPU shortage reason, and no shared
  or unrestricted GPU launch. Check recovery when resources return.
- On a disposable Linux cpuset/cgroup setup, shrink and restore the allowed CPU
  set while test work runs. Check periodic/configuration-triggered discovery,
  saved-intent preservation and release of old ownership before new launches.
  On Windows, exercise the corresponding supported process/topology restriction
  and explicit rejection of masks outside the representable processor group.
- Leave an assignment pending/retrying while changing another group's resources.
  Existing WUs must reconcile. Changed offers must be cancelled and rebuilt
  with residual resources; unchanged offers retain their request/backoff.
  Test budget shrink/growth, zero capacity, GPU changes and late callbacks.

WSL passing these suites establishes Linux compilation and syscall-path checks,
not physical Intel hybrid/big.LITTLE coverage or a real cgroup hot-plug result.


### Native macOS CI

The `macos` job in `.github/workflows/ci.yml` builds the pinned cbang revision
and client with Apple Clang on the Apple Silicon `macos-14` runner. A guarded
macOS-only checkout correction casts the physical-core count to `uint64_t`
before constructing `cb::String`, avoiding an ambiguous Darwin overload in
the pinned topology revision. Remove it when a corrected revision is pinned. Homebrew
OpenSSL is located explicitly through `OPENSSL_HOME`. This is build-only
coverage for shared code and the Darwin legacy-scheduling path; it does not run
strict-affinity integration, ordinary client tests, or exercise real folding.
It does not establish Intel macOS compatibility.


### Native Windows CI

The `windows` job in `.github/workflows/ci.yml` builds official cbang and the
client with MSVC x64 on `windows-2022`, builds and runs the ordinary client
tests, then runs `affinity-fast` and
`affinity-integration`. OpenSSL is built as a static /MT dependency.
The integration runner compiles the actual `CoreProcess.cpp` and executes
its Win32 strict-launch path with disposable child processes. It verifies exact
singleton and expanded masks, inheritance by newly created threads, rejected
masks never executing the payload, and PID/kill/wait lifecycle behaviour.
This is separate from the synthetic startup-diagnostic test. Hosted-runner
coverage does not replace testing real hybrid CPUs or multi-group hardware.
The runner also executes the real strict launcher from a `CREATE_NO_WINDOW`
parent, without `AllocConsole`, checking graceful CTRL_BREAK delivery, exit code,
no kill fallback and restored standard handles. This covers the consoleless
mechanism used by tray/service callers, but not SCM/session-specific deployment.
For release testing, use a disposable service/data directory and test an affinity
change while a CPU WU runs: confirm a graceful core exit and prompt replacement,
not the 60-second forced-kill path. Do not use a production WU for this check.

The Windows native runner respects `OPENSSL_HOME` and `OPENSSL_LIBPATH`.
Without these variables it retains the local `C:/OpenSSL-3.5` layout.

The synthetic allocator runner also links `CPUAllocationPlanner.cpp` directly for
pure planning tests, without cbang or configuration stubs. These cover repeatable
results, unchanged inputs, GPU reservation priority, shared-pool shortages, class
fallback and 441 worker-budget combinations. Both Windows and Linux runners include
this test in the existing fast suite.


CPU configuration policy tests link the complete `CPUConfigValidator.cpp` directly.
The configuration adapter fixture still extracts the Groups boundary to exercise
real Config accessors, candidate construction and topology refresh/reconciliation.
GPU-capacity fixtures call the pure validator directly; the real-client API smoke
and SQLite transaction tests continue to cover application integration and rollback.

### Stock-client database round-trip

Run `python tests/stock-class-compatibility.py --client PATH_TO_STOCK_CLIENT
--affinity-client PATH_TO_CURRENT_CLIENT` (one command). This uses disposable
paused groups: stock removes class keys and edits CPU totals, then the current
client reopens the database, accepts General settings and restarts successfully.
Both real executables are required; no live folding data is used. Linux and Windows
CI build a pinned upstream stock client separately and run this round-trip.

## CPU status serialization

The ordinary suite includes `CPUStatusTests`, linked against the shipped
`CPUStatusJSON` serializer and real cbang JSON objects. Its independent JSON
fixture covers topology/generations, a group potential-SMT prediction, a matching
WU whose actual full-SMT status is false, and a GPU helper shortage. It also
checks that empty WU/helper collections serialize without retaining old entries.
Run it with the ordinary test build and harness commands above.

GPU priority coverage: the scheduling harness compiles the complete `GPUProcessPriorityMonitor.cpp` against a controlled process adapter, covering launch/checkpoint baselines, late progress, cancellation, bounded checks and restoration. Unit progress/publication adapters remain source-extracted. Native Windows strict-affinity tests exercise every allowed class, simulated core reset, startup/progress check gating, readback and removal. Linux native tests cover OTHER/nice 0 launch and thread inheritance with and without affinity, plus stock IDLE/nice 19. Denied overrides must restore verified stock scheduling; injected scheduler/nice restoration failures and readback mismatches reject preparation and reap the child without reporting an affinity rejection. API configuration smoke rejects invalid/privileged priorities before mutation and checks RG persistence across restart. Actual Core 27/28 priority arguments were verified separately in non-folding startup probes; future GPU core versions need support verification before enabling their override.


### Per-WU allocation and saved configuration reconciliation

`wu-planner.cpp` links the complete `WUCPUAllocationPlanner.cpp` directly (no
source extraction), covering the minimum-3 versus minimum-1 recovery and 1,215
minimum/maximum, budget and mixed-SMT cases, including reversed-ID invariance and
more than ten candidates. The Group adapter verifies production
integration, pause decisions and assignment gating. Configuration and real SQLite
transaction fixtures inject reconciliation failures after commit, prove the saved
configuration is accepted, and verify pending state clears after a successful
retry. Rollback failures still preserve the original configuration exception.

Runtime-demand regressions (`cpu-affinity-scheduling/runtime-demand.py`, included
in `affinity-fast`) cover pause, idle/battery/GPU waits, backoff entry/expiry,
Finish and reconciliation failure, plus deleted-RG migration retaining live
ownership. Direct planner tests verify paused demand releases workers and GPU
reservations and cannot activate hybrid class mode. Publication tests keep the
generation stable for unchanged inputs; lifecycle tests verify conflict-checked
pool-only adoption without restarting or modifying stopping-process ownership.

Class-isolation tests assert per-level mask containment under General overcommit,
restricted/empty class masks and unknown topology. Native-source confidence tests
distinguish Windows single-class classification from Linux's heuristic cluster;
synthetic allocation fixtures explicitly inject trusted topology confidence.
Windows strict-launch fault injection covers independent launches with a retained
rejected child, bounded suspended-child retention and eventual cleanup recovery.

The ordinary `AssignmentResourcesTests` suite compiles the real Unit/JSON path:
verified assignment resources replace candidates, malformed/multi-device GPU lists
are rejected before mutation, and persisted preparation states resolve on reload.
Scheduling fixtures cover two independent GPU WUs, per-device shortage isolation,
and release of unselected candidates during DOWNLOAD/CORE. Launch tests reject
multi-device lists rather than selecting a device while owning multiple masks.

Deleted-RG assignment regressions cover post-commit cancellation, rollback retaining
requests, accepted WUs remaining untouched, late callbacks ignored, and old
callbacks not clearing newer requests. The real Unit test also checks abort
completion; SQLite fault injection covers write and deferred COMMIT failures.


Class budgets through WU launch
------------------------------
The RG planner publishes fulfilled worker budgets and whole-core pools separately
for each performance class. The WU planner retains those class slices, including
worker-only reservations for work that is not run-ready. Runnable WUs build their
process mask independently within each slice and combine the masks for one FahCore.
a8/a9 SMT expansion remains local to each slice; other cores use size-matched masks.
Process affinity controls eligible CPUs, not the class chosen by each worker thread.
Invalid class ownership topology suspends CPU work without unrestricted fallback.
The class-planner regression links the shipped RG/WU/execution planners, verifies
single- and multiple-WU composition, and checks 576 independent throughput-oracle
cases. Bounded repair retains its valid baseline at the 128-core/50,000-state limits.

### Packing consolidation

`general-packing-equivalence/run.py` compares the shipped packer and WU policy
against frozen pre-extraction General sources: 10,800 RG comparisons and
17,496 exact pool/worker/mask,
priority, acquisition and report comparisons plus topology/limit cases. It is
part of affinity-fast. The mixed-class independent oracle exercises the same
production packer through WUCPUAllocationPlanner; no prototype class scheduler
is shipped. Frozen reference sources are test fixtures, not production helpers.

## Config field validation

`ConfigTests` is built and run with the ordinary native test suites. It links real Config and cbang JSON code using the shipped group defaults. It covers malformed v6 fields, numeric boundaries, valid saved class intent, dormant General-mode counts, unsupported priority loading and retained saturation/legacy loading behavior. The GPU validation adapter retains pure reservation-capacity tests; API rejection and persistence are checked by `api-configuration-smoke.py`.
