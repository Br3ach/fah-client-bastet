# fah-client Architecture

This document describes how `fah-client` is structured.  Read it before
making non-trivial changes to the client and before writing tests.

## What the client does

fah-client is an event-loop-driven daemon.  At a high level it:

1. Requests a *work unit* (WU) assignment from a Folding@home **Assignment
   Server** (AS).
2. Downloads the WU's data from the **Work Server** (WS) the AS returned.
3. Downloads the science **core** binary for that WU (cached across WUs of
   the same type).
4. Runs the core as a subprocess on the WU.
5. Uploads results to the WS (or to a **Collection Server** (CS) if the
   WS is down).
6. Repeats, scaled to the user's CPU and GPU resources.

The user's browser-based control panel ("the frontend") connects to a
local HTTP/WebSocket port the client serves, and observes/mutates the
client's state.  A separate WebSocket to api.foldingathome.org optionally
links the client to a user account so remote control can also work.

## Major types

The whole client lives in `FAH::Client::` under `src/fah/client/`.

### App (`App.h/cpp`)

The singleton.  Owns:
- libevent base, HTTP client, SSL context.
- SQLite database (`client.db`).
- RSA keypair (signs AS/WS requests).
- The `info` dict, `config`, `groups`, `units`, `gpus` as observable JSON
  children — frontends see these via the `Remote` interface.
- `Server`, `Account`, `GPUResources`, `Cores`, `OS`, `LogTracker`.
- A list of attached `Remote`s.

`App::init` runs command-line/option setup.  `App::run` opens the DB,
calls `loadConfig`, constructs `Groups` / `Units` / `Server`, and enters
the event loop via `os->dispatch()`.

### Server (`Server.h/cpp`)

The HTTP/WebSocket server the local frontend connects to (typically on
`127.0.0.1`).  Routes WS connections to `WebsocketRemote` instances.

### Remote (`Remote.h/cpp`)

Abstract base for any external observer/controller.

- `WebsocketRemote` — a local browser frontend connected to Server.
- `NodeRemote` — a remote browser connected via the Account WS bridge.

A Remote receives every change to the observable JSON tree
(`App::notify` → `Remote::sendChanges`), can send commands back (config
edits, state changes), and watches the log/viewer streams.

### Account (`Account.h/cpp`)

Optional WebSocket connection to `api.foldingathome.org`.  When the user
links the client to an account, this bridge lets remote browsers control
this client.  Has its own state machine: `IDLE → LINK → INFO → CONNECT
→ CONNECTED`, with backoff.

### Groups / Group (`Groups.h/cpp`, `Group.h/cpp`)

A **Group** is a resource pool with its own `Config` (CPU count, GPU
list, pause state, project preferences).  The default unnamed group
inherits its config from the command-line options; additional groups
can be added at runtime by remotes.

`Group::update()` is the **scheduler**.  Each tick it:
1. Triggers each unit's `next()`.
2. Reaps completed units.
3. Honors graceful shutdown.
4. Skips local scheduling and acquisition while paused or waiting, after notifying
   existing units and reconciling changes in global runtime demand.
5. Allocates GPUs, keeping their helper CPU bookkeeping separate from managed
   CPU worker budgets. Legacy scheduling accounts for GPU helper CPUs directly.
6. Distributes remaining CPU workers using the group's managed budget or legacy
   CPU count, subject to each WU's minimum and maximum requirements.
7. In managed mode, partitions the group's owned whole-core pool among enabled
   CPU WUs, reducing workers or waiting when a WU's minimum cannot be met.
8. Sets `pause` on units that didn't get resources.
9. Retains unchanged pending offers; cancels and rebuilds stale offers after
   reconciling existing WUs.
10. Creates a new Unit (calls `Units::add`) if under the max-WU cap and
    any resources are still free.

### Config (`Config.h/cpp`)

A JSON dict subclass with type-checked insertion (rejects keys not in the
defaults template, rejects wrong types).  Each Group has its own Config
loaded from a JSON defaults resource plus DB-persisted overrides plus
command-line options.

### Units / Unit (`Units.h/cpp`, `Unit.cpp`)

A `Unit` is one WU in flight.  It is the largest and most intricate
class in the client.  Its state machine is the `UnitState` enum:

```
                       +---retry on failure---+
                       v                      |
ASSIGN ──► DOWNLOAD ──► CORE ──► RUN ──► UPLOAD ──► DONE
                       │        │       ▲
                       │        │       │
                       │        └─► DUMP┘
                       └─► DUMP ──► DONE  (dumped/expired/failed)
```

`Unit::next()` is the state-machine dispatcher.  Every
trigger (timer event, HTTP response, core exit, pause toggle) calls
`next()`, which checks expiry/pause/waiting and then dispatches to
`assign()`, `download()`, `getCore()`, `run()`, `upload()`, or `dump()`
based on `getState()`.

Per-state functions:

- `assign()` — POSTs a signed JSON request to the next AS in rotation,
  waits for `assignResponse()`.
- `download()` — POSTs to the WS returned by the AS, writes
  `work/<id>/wudata_01.dat`, transitions to `UNIT_CORE`.
- `getCore()` — asks `Cores` to download the WU's science core (cached
  by URL), callback transitions to `UNIT_RUN` and auto-pauses pending
  scheduler approval.
- `run()` — spawns `CoreProcess` with arguments derived from
  CPU/GPU/UUID config.  Starts `TailFileToLog` to mirror core output into
  the client log.
- `monitorRun()` — every second while running: reads `wuinfo_01.dat` for
  progress, reads viewer frames, updates ETA/PPD, detects clock skew.
- `finalizeRun()` — runs after core exits.  Examines `ExitCode`:
  `FINISHED_UNIT`/`INTERRUPTED`/`CORE_RESTART`/`FAILED_*`.  Reads
  `wuresults_01.dat`, signs results, transitions to `UNIT_UPLOAD` (or
  `UNIT_DUMP` on bad/missing data, or retries on `CORE_RESTART`).
- `upload()` — POSTs results.  On failure, retries against the CS list
  before counting the failure.
- `dump()` — submits a "dumped" report and cleans up.

Retry policy (`Unit::retry()`): exponential backoff
`2^min(9, retries)` seconds, with limits that depend on state.  Beyond
the limit the unit is cleaned with reason `"retries"`.

Persistence: every state-changing transition past `UNIT_CORE` writes the
unit's full JSON state to the DB.  On startup, `Units` rehydrates units
from the DB; if a unit was in `UNIT_RUN`, it's reset to `UNIT_CORE` so
the core is verified/redownloaded if needed.

### Core / Cores / CoreProcess

`Cores` is a registry, indexed by core URL.  A `Core` has its own state
machine `INIT → CERT → SIG → DOWNLOAD → TEST → READY` (or `INVALID`).
Cores are cached on disk under `cores/` and shared across all Units that
need the same one. `CoreProcess` delegates legacy launches to `cb::Subprocess`
and owns native strict Windows/Linux launch and lifecycle handling. Strict
launchers apply and verify the CPU mask before the child can execute FahCore.
Unsupported platforms retain legacy unpinned launches.

### GPUResources / GPUResource

`GPUResources` consumes cbang's `GPUInfo`/`GPUDevice` detection abstraction,
combines detected devices with the downloaded GPU support index, and publishes
available GPU resources. Each `GPUResource` stores device identity, driver
descriptors, and a `supported` flag.  Used by Group's scheduler and
referenced by `Unit::run()` to build core arguments.

### OS

Platform-specific (`lin/`, `osx/`, `win/` subdirs).  Provides system idle
detection, battery state, keep-awake hints, and platform-specific exit
handling.  Polls every 2 seconds.

### LogTracker

Captures log lines through the cbang Logger machinery and forwards them
to attached Remotes.

## Event flow

```
   ┌──────────────────────────────────────────────────────────────┐
   │ libevent base (App::base)                                    │
   │                                                              │
   │  ┌────────┐  ┌──────────┐  ┌──────────┐  ┌─────────────────┐ │
   │  │ Signals│  │  HTTP    │  │  Account │  │ Server (frontend│ │
   │  │SIGINT  │  │  client  │  │   WS     │  │   WS in)        │ │
   │  │SIGTERM │  │ (AS/WS)  │  │          │  │                 │ │
   │  └───┬────┘  └────┬─────┘  └────┬─────┘  └────────┬────────┘ │
   │      │            │             │                 │          │
   │      ▼            ▼             ▼                 ▼          │
   │             ┌──────────────────────────────────────┐         │
   │             │       App (observable JSON dict)     │         │
   │             │  config • groups • units • info • gpus│        │
   │             └──┬────────────────┬────────────┬─────┘         │
   │                │                │            │               │
   │      ┌─────────▼────┐ ┌─────────▼──────┐ ┌───▼────────────┐  │
   │      │   Groups     │ │   Units        │ │   Remotes      │  │
   │      │ (scheduler   │ │ (state         │ │ (notified on   │  │
   │      │  per group)  │ │  machines)     │ │  any change)   │  │
   │      └──────────────┘ └────────────────┘ └────────────────┘  │
   │                                                              │
   │                    OS event (2s tick)                        │
   │                    Save event (debounced config write)       │
   └──────────────────────────────────────────────────────────────┘
```

Observable JSON changes reach `App::notify()`, which forwards change lists to
Remotes during ordinary operation. Group-configuration updates batch notifications
while staging and applying changes, then publish the final trees after success
or restoration on failure. Separate final messages are not an atomic network
batch. The frontend observes the client's published JSON state.


## CPU policy, runtime ownership and live affinity

These are three distinct layers. Do not substitute a process mask for a pool,
or overwrite saved policy when runtime capacity changes.

1. **Saved policy (`Config`, `Groups`).** CPU worker counts, performance-class
   counts and physical cores reserved per enabled usable GPU express intent.
   API configuration stages and validates group settings before applying them.
   SQLite rollback restores persistence and the original live group/WU objects.
   Unchanged saved class settings survive temporary topology loss. Notification
   batching publishes final trees, but does not make separate network messages
   atomic or make combined global/group persistence one SQLite transaction. After a
   successful commit, reconciliation failures do not reject the saved settings:
   they log a saved-but-pending warning and coalesce short retries after 5, 15
   and 30 seconds. Success cancels and resets the retry chain. Persistent failure
   retains dirty state for the 300-second topology watcher to retry, even
   without a topology-generation change.
2. **Runtime pools (`CPUResources`, `CPUWholeCorePacking`, `Group`).**
   Positive GPU reservations get separate complete Performance 1 physical cores
   first. Their logical CPUs are excluded from every CPU pool and shared GPU
   helper pool. Zero-reservation GPUs share the remaining Performance 1 pool
   with each other and CPU work. CPU pools own whole cores and remain disjoint
   across groups and WUs. Worker budgets cannot exceed pool logical capacity.
   Whole-core granularity may reduce a budget without changing saved counts.
   Explicit classes take priority, followed by fair General targets, physical
   spreading that preserves achievable General budgets. Surplus physical cores
   remain unowned once each pool has enough distinct cores for its worker demand;
   adding another WU need not restart a peer to reclaim unused cores.
   RG names are ordered lexically for deterministic tie-breaking. The Default
   RG uses the empty name and therefore has highest tie priority, including
   exclusive GPU reservation shortages. When a
   shortage leaves an indivisible remainder, an earlier name can repeatedly
   receive the extra worker or core. Priority does not rotate over time, avoiding
   needless allocation-mask changes and core restarts. This rule does not promise
   equal allocation over time or override class priority and whole-core constraints.
   Missing core maps use deterministic logical pools without claiming physical
   isolation. General hybrid/unknown configurations without classes or GPU
   reservations retain legacy scheduling. General mode specifies a worker
   count, not a P-core/E-core mix. Leaving the process unrestricted preserves
   existing OS scheduling on hybrid CPUs rather than imposing a new core-selection
   policy on General configurations. Explicit performance classes or exclusive
   GPU reservations require managed allocation to enforce resource boundaries.
   Homogeneous supported topology uses managed allocation and the physical-core-first
   process-mask policy below. A class-mode RG with all-zero class counts does not
   activate managed allocation for other General RGs. Pending assignments must
   not delay pool reconciliation. CPU assignments and
   downloads reserve worker budgets but receive physical-core ownership only at
   `UNIT_RUN`, after the folding core is ready; assignment/data/core downloads
   do not displace running cores.
3. **Live process masks (`Unit`, `CoreProcess`).** For a8/a9, N workers at or
   below P owned physical cores use exactly N logical CPUs on separate cores.
   Above P, the process can use the entire owned logical pool without changing N.
   Other core types use matching-size process masks. The client never identifies
   or pins individual GROMACS/OpenMP workers. Full SMT is an advisory warning,
   based on workload-specific measurements, not an automatic worker reduction.
   Group metadata describes topology pools; `potential_full_smt` predicts full
   capacity without assuming a FahCore type. Only per-WU `full_smt` confirms the
   actual core-specific condition. Matching WU metadata takes precedence over
   group predictions in the UI; drafts and unassigned groups use advisory wording.

CPU diagnostics cross a separate presentation boundary. `CPUStatusBuilder` reads
published resources and generation-valid WU execution status into a plain
`CPUStatusSnapshot`; it neither refreshes topology nor changes allocation.
`CPUStatusJSON` serializes that snapshot using the caller's JSON factory so
observable ownership is preserved. `App::updateCPUInfo()` only builds and
publishes the result. Group topology predictions and actual WU core policy
remain distinct in the snapshot and the API.

A smaller process mask still reserves its entire owned physical-core pool.
When settings change, the desired pool can change immediately, but the old
process retains its live reservation until it exits. `Unit::blocksCPULaunch`
checks desired ownership against those live reservations. Shared GPU helpers
are not exclusive owners, but an exclusive GPU reservation must wait for old
CPU or GPU work using its resources. Restart is asynchronous and uses the
normal graceful-stop timeout. It must not be assumed complete when config saves.

Shared GPU affinity requires a usable performance CPU mask. Exclusive GPU
reservation support additionally requires complete fast physical-core topology;
`gpu_cpu_reservation` reports this stricter capability. Missing sibling topology
can therefore permit shared affinity while disabling exclusive reservations.

Strict Windows/Linux launch applies and verifies the exact mask before allowing
core execution. An empty managed mask waits. A positive GPU reservation that
cannot be satisfied waits and publishes its shortage reason, never becomes
shared or unrestricted. A strict affinity rejection refreshes topology and
reconciles pools immediately, with a separate bounded retry cooldown instead of
ordinary WU retries. Windows masks must fit the supported processor group and
Linux IDs must fit `cpu_set_t`; unsupported masks are rejected, never truncated.

The periodic topology probe runs every 300 seconds, and configuration validation
also probes. Running-process changes are not monitored continuously. Before
reconciliation, the OS may restrict an existing process or leave it with an old
mask depending on the platform and change. After detection, conflicting launches
wait for live ownership to be released. This preserves ownership safety, but
does not promise uninterrupted folding or instant detection of every hot-plug
or cpuset change. Real hybrid hardware and live cgroup/processor-group changes
require integration testing beyond synthetic allocator fixtures.

## CPU allocation generations and process ownership

`CPUResources::topologyGeneration` advances when validated availability,
performance/core maps, capabilities or complete reservable cores change.
A probe that returns the same topology does not advance it. Topology discovery
builds a local snapshot before publishing state, so a failed read does not
partially replace the previous topology. Validation still handles inconsistent
maps returned by separate OS reads.

`CPUResources::allocationGeneration` advances only when the published allocation
changes. Changed planning inputs are cached even when they produce the same
allocation; internal search diagnostics do not create a new generation. Group scheduling
stamps each unit's desired pool through `setCPUAffinity`. The unit publishes
CPU execution metadata only when that stamp matches the current allocation
generation. Saved configuration is intent, and is not rewritten by shortages.

Accepted assignment `cpus` supplies the fallback for missing `min_cpus` and
`max_cpus`; runtime reductions do not rewrite those assignment bounds. Scheduled
workers describe the current offer or target; launched workers remain in
`runningAllocation` until the process is reconciled.

`RunningCPUAllocation` captures the worker count, process mask, whole-core
resource pool, CPU placement mode and allocation generation at launch. The modes
are Unmanaged, ManagedCPU, SharedGPU and ReservedGPU; unmanaged GPU work remains
Unmanaged, and an empty managed mask still means blocked work. This record
keeps execution settings fixed. Conflict-checked pool-only adoption is allowed
while the same process mask, worker count and reservation mode remain valid.
Once stopping, its ownership remains frozen until cleanup. Desired and running
records can legitimately describe different generations during reconciliation.
Launch exclusion uses the current running pool, including siblings outside the
process mask. A changed desired mask cannot release the old process's ownership.
Generation inequality is therefore not itself an error.

Affinity-rejection tracking uses the topology generation and rejected process
mask. A different generation or mask resets its consecutive rejection count.
The retry event is armed before fallible notifications, topology refresh or
reconciliation; recovery exceptions cannot strand the unit without a retry.
Exact-mask rejection triggers immediate topology refresh and reconciliation,
with separate bounded backoff rather than an ordinary work-unit retry.
Successful process start clears the consecutive rejection count.


## Configuration capacity and stock rollback

General-only saved-policy oversubscription validation is retained only without
active GPU reservations. Managed runtime allocation still reduces fulfilled workers.
CPU-policy or reservation edits with active GPU reservations validate remaining
total and per-class capacities. Class-policy edits validate those capacities too.
Unrelated edits preserve unchanged allocation intent after topology changes.
Saved counts and runtime fulfilled worker counts remain separate. Within an RG,
`WUCPUAllocationPlanner` accepts ordered WU minimum/maximum requests and returns
whole-core pools and worker counts. General budgeting and partition phases retain
request-vector order through ordinal keys. Class packing maximizes throughput and
uses request-vector order for throughput ties; WU ID spelling does not set priority.
Both paths reject duplicate WU IDs and minimum bounds above maximum bounds.
In General mode, assignments below their minimum are excluded
and the remaining WUs are replanned with the released cores; every failed pass
removes at least one candidate. Spare worker slots are then redistributed within
the final owned pools, bounded by WU maxima and the original RG budget. The initial
unassigned budget is preserved for assignment gating, avoiding requests caused
only by an existing WU's topology-constrained minimum.

Stock clients retain ordinary CPU/GPU settings but strip unsupported `cpu_mode`,
`cpu_class_counts`, `gpu_reserved_cores` and `gpu_priority` on save. Loading the
database is compatible, but those v6 preferences do not survive a stock-client
save. Users must reconfigure them after returning.


### GPU minimum CPU count and affinity pool size

For managed GPU work, `min_cpus` determines the scheduled/accounted helper count,
with a minimum of one. It does not require that many distinct LPs in the helper
pool. The GPU launch path does not pass this count as a CPU worker-count argument.
Helpers may time-share a non-empty shared or reserved pool, including a one-LP
pool when `min_cpus` is two or greater. The one assigned GPU must still
have a non-empty pool. Empty pools wait and never launch unrestricted. This
client policy does not establish the throughput or internal thread requirements
of every GPU core.


### Coherent launch allocation snapshots

Each `Unit::run()` attempt builds one const allocation record containing worker
count, managed/reserved mode, process mask, whole-core ownership pool, allocation
generation and SMT diagnostics. That record drives the empty-pool gate, conflict
checks, arguments, affinity and warnings. A managed CPU pool with a stale scheduler
generation waits for reconciliation. Retries build a fresh record.

Successful `exec()` publishes the record as live `runningAllocation`. A rejected
launch retains its attempted mask separately for affinity-rejection backoff,
without claiming live ownership. Pool-only changes can be adopted after conflict
checks when execution stays unchanged; stopping ownership is frozen until exit.
Snapshot consistency does not guarantee that OS topology stayed unchanged: strict launch still verifies the mask on the OS.

### Pure allocation planning

`CPUAllocationPlanner::plan()` takes a validated topology value and resource-group
policy requests and returns a complete desired allocation. It has no OS discovery,
configuration, logging, generation or process-lifetime dependencies. Its disposable
builder executes GPU reservations, class allocation, General worker targets,
physical spreading up to worker demand in that order. Surplus cores stay unowned. Worker budgets stay
separate from physical-pool logical capacity. Whole-core search is best effort:
128 assigned cores for fixed-target search; constrained search also limits
physical cores and consumers to 128. Both allow 50,000 visited states; constrained
packing additionally limits traversal to 512 chunks, including pending consumers. Limits retain a valid baseline
or the best safe candidate found. DEBUG reports distinguish limits, infeasibility,
improvement and an unchanged maximal packing. General WU recovery preserves the
first search-limit report across retries; otherwise it reports the final pass.
Reports describe that pass, not necessarily the final allocation. Speculative
spreading trials do not emit reports. GPU reservation priority and CPU shortage
ties use lexical resource-group ordering.
The empty-name Default RG precedes all named RGs.
GPU IDs within a group are also ordered lexically. Insertion/load order does not
affect shortage priority.

`CPUResources` adapts saved configuration into requests, invokes the planner and
publishes its result. It owns topology discovery, generation stamps and change-only
diagnostics. `Unit` still owns running-process reservations and launch conflicts.
The pure planner tests link its production implementation without topology or
configuration stubs; the existing allocator fixtures also exercise publication.

Allocation publication stores one `CPUAllocationPlanner::Result` in
`CPUResources`. Planning and validation finish before a non-throwing swap replaces
that result, then `allocationGeneration` advances. A staging failure retains the
previous complete allocation and its generation. Diagnostics run after publication
and compare against the old result returned by the swap. This is coherent within
the client event loop, not a cross-thread synchronization primitive.


### CPU configuration validation

`Groups::validateCPUConfiguration()` refreshes topology and reconciles existing
saved policy when necessary, checks JSON values and materializes candidate `Config`
objects. It passes plain current/proposed requests and a topology snapshot to
`CPUConfigValidator`. The validator has no application, JSON, database, logging or
OS dependencies and does not mutate its inputs. Its phases analyze active GPU
reservation changes, derive reservation-adjusted capacities, validate CPU/class
requests and enforce changed-policy aggregate limits. Rejections return an error
for the Groups adapter to log and throw before transactional application.
Unchanged stale class intent, shared-GPU enablement checks, wide class totals and
legacy General oversubscription retain their existing semantics.

Strict process setup is isolated in `win/CoreProcessLaunch.cpp` and
`lin/CoreProcessLaunch.cpp`. `CoreProcess::execStrict()` dispatches setup and
publishes child ownership only after the platform checks succeed. Local RAII
owners close temporary Windows handles/environment blocks and Linux status
pipe descriptors. Platform rejection paths retain their terminate-and-reap
cleanup, and shared lifecycle methods own successfully launched children.

`Unit::run()` retains scheduling checks, WU filesystem preparation, launch and
success-only ownership publication. `buildCoreArgs()` and `createCoreProcess()`
only construct FahCore arguments and configure its process from the captured
allocation; neither refreshes topology nor recomputes desired ownership.

CPU launch conflict rules live in the pure `CPUOwnershipPolicy::blocksLaunch()`
helper. `Unit` checks process presence and supplies GPU identity plus captured
allocations; tests link the policy directly without locating its source text.
Allocator fixtures share a Python compiler driver and source manifest on both
platforms. SCons calls that driver directly; shell wrappers only forward to it.

### GPU process CPU priority

`gpu_priority` is an optional resource-group setting, shared by all GPU WUs in that group. Empty means stock idle launch and no monitoring override. CPU WUs and affinity allocation are unaffected. Windows permits idle, below-normal, normal, above-normal and high. Realtime is deliberately excluded because it can impair system responsiveness. The process class is applied at launch, checked every five seconds during initialization, restored after progress advances beyond the pre-launch checkpoint baseline (or beyond the first valid shared record when no pre-launch record exists), and then checked every five percentage points of actual progress. Changes apply live; removing the override restores idle. Repeated reset/failure messages are limited per process/state.

Linux permits other-low and other-normal. A separate child starts with SCHED_OTHER/nice 0 and supported Core 27/28 receive --priority low/normal. This avoids trying to raise nice after an idle launch. Verified Core 27/8.2.1 defaults set nice 19 while preserving the inherited scheduler; Core 28/8.3.1 defaults also select SCHED_IDLE. Explicit low/normal arguments set nice 10/0 without changing the scheduler, so both the SCHED_OTHER launch attributes and the argument are needed. These observations cover startup, not every later simulation thread. High/realtime core arguments map to negative nice values rather than realtime scheduler classes and normally require additional privileges; they are not offered on Linux. If launch attributes cannot be applied, stock idle scheduling is restored and verified without relaxing affinity. If neither the override nor verified stock fallback can be established, process preparation is rejected; it is not an affinity rejection. Unsupported cores retain stock behavior. At first reported work, all existing threads are read back; mismatch is advisory, with no periodic thread rewriting. Linux setting changes take effect on the next launch. GPU priority warnings are published on WUs and displayed in rows/details. No override, failures, or missing GPUs never discard saved intent. Priority does not reserve CPUs against other OS processes and does not change GPU hardware scheduling.

Unpaused downloads/uploads remain active. A run-ready WU without a process is
inactive during affinity-rejection backoff or while its managed mask is empty,
so those waits do not keep the machine awake. Affinity launch failures also
appear in failure status without consuming ordinary WU retries. On Windows,
consoleless strict-process stop temporarily attaches to the hidden child console
to deliver CTRL_BREAK, then detaches and restores the client standard handles. Final planning
validation checks exclusive GPU masks against whole fast cores and their request
counts, disjoint device ownership and exact reserved-set coverage. Shared masks
may overlap CPU pools and each other but must exclude exclusive reservations.
An invariant failure clears launch masks and retains managed, fail-closed state.

### Saved entitlement and runtime demand

Configuration validation checks saved CPU/class/GPU reservation entitlements,
including paused groups. Runtime planning separately receives `wantsResources`:
pause, idle/battery/GPU waits and group-wide failure backoff release desired
workers and GPU reservations without rewriting settings. Finish retains demand
until existing folding work completes; upload-only groups need no CPU pool.
Local group polls reconcile globally on eligibility transitions, including
backoff expiry, and re-arm before fallible reconciliation. A paused class group
cannot activate managed allocation for active General groups on a hybrid CPU.
Group-wide failure waits also pause existing cores before their released desired
resources can be reused; launch conflicts retain actual ownership until exit.
Inactive helper groups do not report their deliberately empty masks as shortages.

Within an eligible RG, exclusive GPU reservations are per enabled usable device,
not per assigned or running GPU WU. A device keeps its planned reserved cores
during assignment retries, server delays and preparation gaps, even when no
FahCore is running. CPU folding cannot borrow that capacity during those gaps.
This intentional stability policy avoids reclaiming CPU cores and disrupting
CPU jobs each time GPU work arrives. Group-level inactivity still releases the
desired reservation as described above; live processes retain ownership until exit.
Per-GPU dynamic release is not implemented. It would require explicit demand
transitions, launch-conflict checks and a grace period to avoid allocation churn.

Allocation generations advance only when topology or placement inputs change;
unrelated settings edits reuse the current allocation. A running process with
unchanged execution settings can adopt pool-only changes without restarting,
but acquisition checks all other live ownership first. Stopping processes and
units migrated from a deleted RG retain live ownership until safe adoption or
exit. Transient
missing topology continues to fail closed rather than retain potentially stale
physical-core information.

### Class shortages and topology confidence

Each performance class is allocated independently, before General demand uses
remaining capacity. Oversubscribed classes reduce workers within their selected
level, possibly to zero. Missing saved levels never migrate workers to another
level. An unknown or physically ambiguous class map blocks class CPU work while
General groups can consume safe remaining resources. General overcommit cannot
cancel class boundaries. Saved validation remains separate from runtime capacity.

Windows EfficiencyClass supplies structural single-class classification. Linux's
metric clustering can combine different core types within its threshold; one
cluster alone therefore does not enable homogeneous managed General allocation
or label every core as fastest for GPU reservations. Multiple validated Linux
levels still support class allocation and GPU helpers. Confidence comes from
cbang classification rather than additional FAH OS topology detection.

Windows rejected suspended children are retained independently in eight bounded
cleanup slots. Failed cleanup of one child does not block unrelated strict
launches. Exhausting all slots fails closed before another process is created;
cleanup is retried on subsequent launch attempts and at client shutdown. Launch
rejection polls termination without blocking the event loop; an unconfirmed exit
retains its handle even when TerminateProcess succeeded. Inherited environment
variables and overrides use the same case normalization.

### Candidate and assigned GPU resources

An ASSIGN unit offers candidate GPU IDs to the server. After signature/request
verification, the assignment's resource list replaces those candidates before
DOWNLOAD, allowing other GPUs to receive work during download/core preparation.
The current FahCore launcher supports zero GPUs for CPU work or exactly one GPU
for GPU work. Multi-device, malformed and unoffered GPU selections are rejected
before candidate resources change. Reload and core-ready handling validate the
persisted assignment too; multi-device lists cannot acquire launch ownership.

Deleting an RG migrates accepted WUs, preserving frozen running ownership.
Pending ASSIGN placeholders are aborted only after the configuration transaction
commits; rollback preserves their requests. Default then requests fresh work
under its own policy. Response callbacks must match the currently active HTTP
request, so cancelled/late callbacks cannot accept stale assignments or release
a newer request. Assignment cleanup is scheduled before cancellation/cleanup.

Pending assignments do not consume existing-WU worker budgets or whole-core
ownership. Reconciliation compares residual CPU/GPU resources with the captured
assignment offer. Changed offers are cancelled and recreated; unchanged offers
retain their request and backoff. No replacement is created without resources.

Accepted CPU WUs excluded from runtime allocation suppress new CPU offers until
existing work becomes schedulable or leaves the execution/preparation states.
This does not reserve idle capacity or prevent redistribution among existing
WUs. Independent GPU offers remain available with zero advertised CPU workers.

When fixed-target whole-core packing cannot fulfill every computed target, bounded
search maximizes fulfilled workers, then max-min fulfilled counts, then key-order
ties. RG keys are lexical names; General WU keys preserve request-vector order.
The existing safe packing is the baseline; the bounds documented under pure
allocation planning remain best-effort. Reports distinguish improvement from
search exhaustion.
General WU minimum exclusion and redistribution remain in the WU caller after
packing. Class requests constrain the total workers across their slices.

Accepted CPU work in DOWNLOAD or CORE is resource-paused when excluded from
allocation, preventing further preparation until its minimum becomes satisfiable.
In-flight transfers/shared core downloads may finish; they are not cancelled.
GPU preparation and upload/dump states retain their existing behavior.
Resource pausing does not extend an accepted assignment's deadline.

### Runtime orchestration boundaries

`Unit` reads core progress and publishes priority diagnostics. Its
`GPUProcessPriorityMonitor` owns per-launch baselines, check timing and platform
priority decisions; it has no WU lifecycle, JSON or logging dependency.
`Group::update()` applies eligibility and existing-WU scheduling before calling
`updateAssignmentOffer()` with remaining resources, enabled WUs and the WU count.
The private helper filters acquisition candidates and reconciles pending offers;
it does not replan or change existing ownership.


### Class budgets through WU launch
The RG planner publishes fulfilled worker budgets and whole-core pools separately
for each performance class. The WU planner retains those class slices, including
worker-only reservations for work that is not run-ready. Runnable WUs build their
process mask independently within each slice and combine the masks for one FahCore.
a8/a9 SMT expansion remains local to each slice; other cores use size-matched masks.
Process affinity controls eligible CPUs, not the class chosen by each worker thread.
Invalid supplied class ownership topology suspends CPU work without unrestricted fallback.
The class-planner regression links the shipped RG/WU/execution planners, verifies
single- and multiple-WU composition, and checks 576 independent throughput-oracle
cases. Bounded packing retains a valid baseline or best candidate at its search limits.

`CPUWholeCorePacking` owns whole-core partition, local repair, bounded search,
spreading and invariant validation. RG policy prepares fair targets and resource
constraints; WU policy prepares minimum/maximum bounds, readiness and acquisition
accounting. One search traversal handles fixed single-resource targets and joint
resource constraints. General minimum-exclusion and redistribution stay in the
WU caller; they are not replaced by the class throughput objective.
`CPUExecutionPlan` consumes ownership and applies core-specific execution policy
within each slice. Published `class_allocations` preserve class indices, worker
budgets, capacities and SMT diagnostics. RG diagnostics predict potential SMT;
actual WU core policy takes precedence. The frozen General differential fixture
checks exact pools, workers, masks, ordering and bounded-search outcomes. WU
recovery deliberately retains earlier search-limit diagnostics without changing
those allocation decisions.

Physical-core indivisibility requires a usable sibling map. When discovery supplies
no physical-core map, the inherited logical-CPU singleton fallback remains in use;
class boundaries and worker budgets still apply, but unknown SMT siblings cannot
be kept together. This consolidation preserves that availability policy, including
General-mode fallback, rather than treating missing topology as new evidence of
physical-core separation.
