# Process rollback feasibility

## Result

This experiment did not restore execution. It demonstrates that Windows Process Snapshotting can preserve a readable copy of a controlled process state; it does not provide an inverse operation that makes the live process resume from that copy.

The independent x64 harness consistently found four process threads: its three cooperative simulation threads and one additional thread whose reported start routine belongs to `ntdll.dll` (the thread's purpose was not identified). The PSS snapshot contained contexts for all four, but the additional thread had no harness-defined checkpoint point. The controller therefore refused restoration. No memory or thread context was written back.

This is a fail-closed result, not a savestate prototype. Do not integrate process restoration into SifuTAS or continue adding actor fields on the basis of this experiment. Treat gameplay savestates as out of scope unless a materially different cooperative architecture first passes full rollback and graphics tests.

## Harness

`tools/procrollback/rollback_test.cpp` starts a separate child process. Three registered threads advance a deterministic simulation behind generation-tagged barriers. The state includes RNG, logical timers, thread-local counters, a linked heap list, and allocation/free counters. The parent captures a PSS VA clone, thread entries and contexts, then reads the live process and clone independently.

Across five final runs:

- PSS preserved the checkpoint A payload (ticks, RNG, timers, heap list, and allocation counters): its hash matched both immediately before capture and in the clone.
- Branch B advanced all three workers from 120 to 220 ticks and changed the live hash while the clone retained A.
- Branch C advanced them to 310 ticks. The heap list remained capped at 48 nodes while allocations rose from 72 at A to 186 at C and frees from 24 to 138, proving post-checkpoint allocation and deallocation activity in the simulated branch.
- At checkpoints A/B/C, each worker's externally reported TLS counter was 120/220/310. The payload's allocation/free/node counts were 72/24/48, 132/84/48, and 186/138/48 respectively.
- PSS reported four thread contexts each run. Three began in `rollback_test.exe`; the fourth began at `ntdll.dll` address `0x00007ffc039a4990` in these runs and was not registered with the simulation barriers.
- Capture copied about 8.71 MB of committed private regions across 12 regions. Five capture durations were 6.282, 5.474, 7.023, 6.043 and 6.041 ms (6.17 ms mean). This does not include any restore cost; no restore occurred. PSS copy-on-write storage overhead was not measured.
- The restore gate refused every run with `unregistered process thread(s) lack cooperative checkpoint points`.

The per-thread TLS counters were read from the harness's external shared control mapping at each barrier; the hash excludes them, and the test does not inspect TLS in the PSS clone. The test's PASS label covers only payload capture, clone preservation, branch divergence, and safe refusal. It does not mean rollback passed. Allocation differences were exercised, but not reversed. RNG state was captured in the payload, and thread contexts were captured by PSS, but neither was restored. There was no repeated load or post-restore execution.

Build and run with MinGW-w64 x64:

```powershell
g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -Wall -Wextra -municode `
  -o tools\procrollback\build\rollback_test.exe `
  tools\procrollback\rollback_test.cpp -lkernel32 -luser32 -lpsapi
.\tools\procrollback\build\rollback_test.exe
```

The executable is a local harness artifact under ignored `build/`; it is not committed.

## Why the Windows snapshot does not restore a process

PSS can create a VA clone and return thread metadata and captured contexts. The clone is a separate snapshot view. Windows does not provide a PSS operation that replaces a running process's address space, rewinds its threads to those contexts, or restores kernel and driver state. The controller would need to build that behavior itself.

Copying pages back is insufficient. A correct live restore must first stop all target activity at a coherent boundary, reconcile reserved/committed mappings and page protections, undo allocations and free operations, restore all stacks/TLS and processor state, account for threads created or exited after A, and safely resume every thread. Arbitrary suspension does not establish a coherent boundary. The harness's extra thread demonstrates that even a small target has runtime-managed threads outside its explicit simulation barrier.

PSS's clone and the harness's private-region copy cover only a subset of state. File handles, pending I/O, clocks, kernel synchronization objects, window-system state, audio, and GPU/driver resources are external to ordinary private memory. The required D3D11 test was not run because the lower-level memory/thread rollback gate failed. There is no graphics restoration strategy established here.

The precedents confirm that a snapshot file is only one part of rollback. DMTCP quiesces user threads, handles socket state, records kernel descriptor state, and reconstructs memory and threads through a restart path; its signal, `/proc`, and Linux descriptor mechanisms do not transfer directly to Windows. libTAS recreates threads, restores TLS/register contexts, and models video/audio; its documentation also calls out native-driver state that is outside game memory. Hourglass is a Windows TAS tool with separate shared, debugger, and per-target components; its architecture is application/tool-owned, not a generic PSS restore API. These systems rely on broad control over the target runtime and external interfaces that SifuTAS does not have. [DMTCP architecture](https://github.com/dmtcp/dmtcp/blob/main/doc/src/architecture-of-dmtcp.tex), [libTAS internals](https://clementgallet.github.io/libTAS/guides/how/), [Hourglass source](https://github.com/TASEmulators/hourglass-win32).

Sifu is a closed-source native Windows game with engine, middleware, OS, and graphics-driver threads. SifuTAS does not currently own those boundaries or virtualize those external services. Building that control layer would be a separate, substantially larger project, and the present evidence does not justify continuing toward arbitrary-frame rollback.

## Test coverage and verdict

| Requirement | Result |
| --- | --- |
| PSS memory capture and retained clone | Demonstrated for this harness |
| Live branch differs from checkpoint while clone stays at A | Demonstrated |
| Heap allocation and deallocation after A | Demonstrated, not reversed |
| RNG state retained in cloned payload | Demonstrated by payload hash, not restored |
| TLS values | Recorded externally at the barrier; clone TLS not inspected or restored |
| Thread contexts captured | Four reported; no context was restored |
| Complete cooperative thread set | Failed: one additional unregistered thread starting in `ntdll.dll` |
| Memory rollback | Not attempted |
| Resume from saved instruction/stack contexts | Not attempted |
| Repeated restoration and failure injection | Not tested |
| D3D11/GPU restoration | Not tested; blocked by failed gate |
| Sifu process or DLL | Not touched |

Feasibility verdict: **C — not viable under the current constraints**. Windows PSS is useful for process inspection and capture, but this pass did not establish a safe restoration path. The thread-coverage failure, lack of a supported inverse PSS operation, and unhandled external graphics/system state mean SifuTAS should stop pursuing arbitrary-frame process savestates under its current user-mode hook architecture. Preserve the existing time-control features. Keep F7/F8 explicitly described as legacy partial actor snapshots, not gameplay savestates.

References:

- [PssCaptureSnapshot](https://learn.microsoft.com/windows/win32/api/processsnapshot/nf-processsnapshot-psscapturesnapshot)
- [PSS_CAPTURE_FLAGS](https://learn.microsoft.com/windows/win32/api/processsnapshot/ne-processsnapshot-pss_capture_flags)
- [PSS_THREAD_ENTRY](https://learn.microsoft.com/windows/win32/api/processsnapshot/ns-processsnapshot-pss_thread_entry)
- [SuspendThread](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread)
- [libTAS savestate architecture](https://libtas.sourceforge.io/docs.html#savestate)
