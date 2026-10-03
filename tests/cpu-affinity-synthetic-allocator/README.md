# Executable allocator regressions

These tests compile the shipped CPUResources.cpp, substituting only SystemInfo, Groups, Group, Config and logging. The suite covers ordinary stability, cpuset/class shortage, recovery, incompatible saved class-vector length, fair capacity loss, sibling ownership, unavoidable split and loss of hard affinity. Exhaustive coverage checks 969 three-RG budgets on eight uniform SMT cores plus eight singleton E cores. Assertions are required.

Run sh run.sh with a C++17 compiler, or run.cmd in a Visual Studio Developer Command Prompt. This is not a linked FAH client or OS-affinity integration test.

V2 adds rejected partial/overlapping sibling maps, internal singleton fallback, mixed class/general ownership and 220 mixed-width budgets checked against an independent co-location feasibility oracle.
