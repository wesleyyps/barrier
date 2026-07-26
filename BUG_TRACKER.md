# Barrier Bug Tracker & Remediation Ledger

This document strictly tracks the remediation of structural and logical bugs identified during static analysis (e.g., via `clang-tidy`). Because this is a highly concurrent, multithreaded application, fixing bugs requires strict control to trace back any regressions.

## Bug Categories

| Category Code | Description | Severity |
|---|---|---|
| **VIRT_CALL** | `clang-analyzer-optin.cplusplus.VirtualCall` - Calling a virtual method during construction or destruction. Leads to undefined behavior because the vtable is incomplete. | HIGH |
| **EMPTY_GET** | `bugprone-standalone-empty` - Calling `.empty()` on a container without checking the result. Usually indicates the developer meant to call `.clear()`. | HIGH |
| **IMPLICIT_BOOL** | `readability-implicit-bool-conversion` - Implicit conversions from integers/enums to boolean. Can hide logic errors in bitwise operations. | MEDIUM |
| **NULL_DEREF** | `clang-analyzer-core.NullDereference` - Dereferencing a null pointer. | CRITICAL |
| **USE_AFTER_FREE**| `clang-analyzer-cplusplus.NewDelete` - Use of memory after it is released. | CRITICAL |
| **OOB_ARRAY** | `clang-analyzer-security.ArrayBound` - Out of bound access to memory preceding or after the heap area (Buffer Overflow). | CRITICAL |
| **INSECURE_API**| `clang-analyzer-security.insecureAPI.strcpy` - Call to function 'strcpy' is insecure as it does not provide bounding. | HIGH |
| **MEM_LEAK** | `clang-analyzer-osx.cocoa.RetainCount` - Potential leak of a CoreFoundation/Objective-C object. | MEDIUM |
| **NULL_API** | `clang-analyzer-nullability` - Passing or returning a null pointer to/from an API that requires non-null. | HIGH |
| **DEAD_STORE**| `clang-analyzer-deadcode.DeadStores` - A value is assigned to a variable but is never read. | LOW |

---

## Remediation Log

When a bug category is addressed, it must be logged here with the files affected, the commit hash, and any relevant context for traceability.

| Date | Category | Files Affected | Commit Hash | Context / Notes | Status |
|---|---|---|---|---|---|
| 2026-07-22 | VIRT_CALL | `Clipboard.cpp`, `ClientProxy1_0.cpp`, `ServerApp.cpp` | c4ec93c0 | Refactoring initialization to avoid virtual dispatches during object lifecycle transitions. | **COMMITTED** |
| 2026-07-22 | EMPTY_GET | `Clipboard.cpp`, `Server.cpp` | c4ec93c0 | Suppressed false-positives where `empty()` actually empties the clipboard instead of checking it. | **COMMITTED** |
| 2026-07-23 | NULL_DEREF | `OSXKeyState.cpp` | 6085d952 | Added null check before dereferencing mask pointer. | **COMMITTED** |
| 2026-07-23 | USE_AFTER_FREE | `ArchNetworkBSD.cpp` | 6085d952 | Added `[[noreturn]]` to `throwError` to fix false-positive analyzer paths. | **COMMITTED** |
| 2026-07-23 | OOB_ARRAY | `ArchNetworkBSD.cpp`, `ProtocolUtil.cpp` | 6085d952 | Fixed negative allocation bounds in `pollSocket`. Evaluated `ProtocolUtil.cpp` as false positive. | **COMMITTED** |
| 2026-07-23 | INSECURE_API | `IKeyState.cpp`, `Server.cpp`, `OSXKeyState.cpp` | 6085d952 | Replaced `strcpy` and `bzero` with safe `memcpy` and `memset`. | **COMMITTED** |
| 2026-07-23 | MEM_LEAK | `OSXKeyState.cpp`, `OSXDragSimulator.mm`, `OSXPasteboardPeeker.mm`, `OSXScreen.mm` | 6085d952 | Fixed missing `CFRelease` calls, autoreleases, and matched CoreFoundation 'Get' rule ownership. | **COMMITTED** |
| 2026-07-24 | NULL_API | `OSXClipboard.cpp`, `OSXDragView.mm` | 54f9ddf4 | Fixed null violations in Pasteboard API integrations. | **COMMITTED** |
| 2026-07-24 | DEAD_STORE | `OSXClipboard.cpp`, `ArchMultithreadPosix.cpp`, `ArchNetworkBSD.cpp`, `OSXKeyState.cpp` | 54f9ddf4 | Handled compiler warnings for unused status codes returned by APIs, particularly those stripped in Release mode by `assert`. | **COMMITTED** |
| 2026-07-24 | USE_AFTER_FREE | `OSXKeyState.cpp` | 54f9ddf4 | Fixed EXC_BAD_ACCESS caused by improper memory management of `TISInputSourceRef`. | **COMMITTED** |

| 2026-07-24 | IMPLICIT_BOOL | `OSXKeyState.cpp`, `VersionChecker.cpp`, `barriers.cpp`, `ZeroconfBrowser.cpp` | 84be6dab | Replaced implicit boolean conversions with explicit comparisons or correct types. | **COMMITTED** |
| 2026-07-24 | USE_NULLPTR | `ZeroconfBrowser.cpp`, `IDataSocket.cpp`, `ClientProxy1_0.cpp`, `ArchTimeUnix.cpp`, `ClientTaskBarReceiver.cpp` | 90e3fb4e | Replaced legacy NULL macros and 0s with modern C++ nullptr. | **COMMITTED** |
| 2026-07-24 | AVOID_C_ARRAYS | `ClientProxy1_0.cpp` | 90e3fb4e | Converted C-style array to std::array. | **COMMITTED** |
| 2026-07-25 | BUGPRONE_EXCEPTIONS | `TCPSocket.cpp`, `XSocket.cpp`, `ArchMultithreadPosix.cpp`, etc. | 32786811 | Handled empty catch blocks and exception escapes. | **COMMITTED** |
| 2026-07-25 | BUGPRONE_SWITCHES | `ProtocolUtil.cpp`, `ClipboardChunk.cpp`, `OSXKeyState.cpp`, etc. | 32786811 | Added missing default cases to integer switch statements. | **COMMITTED** |
| 2026-07-25 | BUGPRONE_NARROWING | `ArchNetworkBSD.cpp`, `TCPSocket.cpp`, `ClipboardChunk.cpp` | 32786811 | Fixed implicit narrowing conversions causing potential data loss. | **COMMITTED** |
| 2026-07-26 | MODERNIZE_DEFAULTS | Across codebase (e.g. `App.cpp`, `ArchDaemonNone.cpp`) | 701ad505 | Applied `modernize-use-equals-default` to utilize compiler-optimized constructors/destructors. | **COMMITTED** |
| 2026-07-26 | READABILITY_ELSE_RETURN | Across codebase (e.g. `String.cpp`, `Server.cpp`) | 701ad505 | Applied `readability-else-after-return` to reduce unnecessary nesting and improve flow control. | **COMMITTED** |
| 2026-07-26 | MODERNIZE_C_CASTS | Across codebase (66 files) | 3494eb86 | Replaced legacy C-style casts with static_cast/const_cast/reinterpret_cast. | **COMMITTED** |
| 2026-07-26 | MODERNIZE_USE_AUTO | Across codebase (66 files) | 3494eb86 | Replaced explicit types (like iterators) with `auto` where types are obvious from RHS. | **COMMITTED** |
| 2026-07-26 | MODERNIZE_LOOP_CONVERT | Across codebase (66 files) | 3494eb86 | Converted index-based or iterator-based `for` loops to modern C++11 range-based loops. | **COMMITTED** |
| 2026-07-26 | MODERNIZE_USE_NULLPTR | Across codebase (82 files) | e354b4ba | Completely eliminated nearly 600 legacy `NULL` and `0` assignments, replacing them with C++11 `nullptr`. | **COMMITTED** |
| 2026-07-26 | MINOR_READABILITY | Across codebase (35 files) | ff5c0bf1 | Consolidated fix for `isolate-declaration`, `uppercase-literal-suffix`, `concise-preprocessor-directives`, and `exception-copy-constructor-throws`. | **COMMITTED** |
