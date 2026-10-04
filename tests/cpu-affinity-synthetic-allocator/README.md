# Executable allocator regressions

These tests compile the shipped CPUResources.cpp, substituting only SystemInfo, Groups, Group, Config and logging. The suite covers ordinary stability, cpuset/class shortage, recovery, incompatible saved class-vector length, fair capacity loss, sibling ownership, unavoidable split and loss of hard affinity. Exhaustive coverage checks 969 three-RG budgets on eight uniform SMT cores plus eight singleton E cores. Assertions are required.

Run sh run.sh with a C++17 compiler, or run.cmd in a Visual Studio Developer Command Prompt. This is not a linked FAH client or OS-affinity integration test.

V2 adds rejected partial/overlapping sibling maps, internal singleton fallback, mixed class/general ownership and 220 mixed-width budgets checked against an independent co-location feasibility oracle.

The pure reservation checker is compared in 2,698 cases against an independent brute-force
assignment reference. An injected duplicate CPU verifies that the allocator
clears all CPU masks while remaining managed, preventing unrestricted launches.

Optional matched benchmark (MSVC developer shell or Linux with g++):
`python benchmark-reservation.py --baseline 2ccf747`
It compiles the baseline and current production checker bodies together at O2,
checks identical results and visit counts, and times three runs of 128 seeded
uniform/mixed-width workloads. These measure the checker, not whole allocator
latency or folding performance. The search bound remains a per-candidate limit.

Recorded Windows MSVC /O2 result: baseline median 191.340 ms, extracted checker
median 194.336 ms per batch; one workload reached the bound. This does not show
a meaningful speed improvement. Results depend on machine and compiler.

Recorded Linux g++ -O2 result: baseline median 146.453 ms, extracted checker
median 145.814 ms per batch; the same workload reached the bound, with identical
outcomes and visit counts. No meaningful timing change was observed.
