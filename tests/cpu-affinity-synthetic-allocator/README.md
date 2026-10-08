# Whole-core allocator and a8/a9 process pools

Run `run.cmd` in a Visual Studio Developer Command Prompt, or `sh run.sh`
with a Linux C++17 compiler. Both wrappers forward to the shared `run.py`
driver, which SCons invokes directly. Run it with Python to use the same
source list on either platform. The production allocator and execution plan are
compiled with topology/configuration adapters. Assertions remain enabled.

Coverage includes 441 two-group worker budgets on 8C/16T, disjoint physical
ownership, explicit classes, preserved intent during topology loss, capability
loss, per-enabled-GPU reservations, shared helpers, partial cpusets and mixed
SMT widths. Pure execution-plan cases cover a8 and a9 at every worker count on
8C/16T and 7C/14T, expanded pools above physical capacity, advisory full-SMT
status, oversubscription, unknown/overlapping core maps and simultaneous WUs.

These are deterministic allocation regressions. They do not measure folding
throughput. Real process launch enforcement is tested by `strict-affinity`.
No worker detection or individual thread-affinity controller remains.
