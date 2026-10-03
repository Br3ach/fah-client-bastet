# SQLite transaction and notification failure injection

Run python tests/cpu-affinity-transaction/run.py --cbang /path/to/pinned/cbang from the repository root. On Windows use a Visual Studio Developer Command Prompt; on Linux provide cc and a C++17 compiler. The runner checks source hashes, compiles the pinned SQLite source, and extracts the shipped Groups::configure and App notification methods plus pinned Database begin/commit/rollback and Transaction commit/destructor bodies. No copied implementation of those function bodies is maintained in the harness.

Adapters supply JSON, Config, group storage, WU membership, remote capture, database SQL execution and projection/validation. The tests isolate application/rollback/publication; they do not test proposal validation, actual cbang observable parent hooks, the entire Database implementation, live sockets or the fully linked FAH client.

A SQLite ABORT trigger injects a group-save failure after creation/deletion/migration. A deferred foreign-key violation injects a real COMMIT failure. Both verify original database rows, live object/config identity, WU membership, removal of newly created groups, cleared flags, connection usability, one rollback, absence of intermediate remote updates and successful retry. Final trees are separate compatible packets, not an atomic packet batch.

V3 adds validation-rejection notification coverage and reconciliation failure injection after both commit and rollback. Validation/topology remains an adapter in this harness.
