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

---

## Remediation Log

When a bug category is addressed, it must be logged here with the files affected, the commit hash, and any relevant context for traceability.

| Date | Category | Files Affected | Commit Hash | Context / Notes | Status |
|---|---|---|---|---|---|
| 2026-07-22 | VIRT_CALL | `Clipboard.cpp`, `ClientProxy1_0.cpp`, `ServerApp.cpp` | c4ec93c0 | Refactoring initialization to avoid virtual dispatches during object lifecycle transitions. | **COMMITTED** |
| 2026-07-22 | EMPTY_GET | `Clipboard.cpp`, `Server.cpp` | c4ec93c0 | Suppressed false-positives where `empty()` actually empties the clipboard instead of checking it. | **COMMITTED** |
| 2026-07-23 | NULL_DEREF | `OSXKeyState.cpp` | pending | Added null check before dereferencing mask pointer. | **PENDING COMMIT** |
| 2026-07-23 | USE_AFTER_FREE | `ArchNetworkBSD.cpp` | pending | Added `[[noreturn]]` to `throwError` to fix false-positive analyzer paths. | **PENDING COMMIT** |
| 2026-07-23 | OOB_ARRAY | `ArchNetworkBSD.cpp`, `ProtocolUtil.cpp` | pending | Fixed negative allocation bounds in `pollSocket`. Evaluated `ProtocolUtil.cpp` as false positive. | **PENDING COMMIT** |
| 2026-07-23 | INSECURE_API | `IKeyState.cpp`, `Server.cpp` | pending | Replaced all instances of `strcpy` with safe `memcpy` using known bounds. | **PENDING COMMIT** |
| 2026-07-23 | MEM_LEAK | `OSXKeyState.cpp`, `OSXDragSimulator.mm`, `OSXPasteboardPeeker.mm`, `OSXScreen.mm` | pending | Fixed missing `CFRelease` calls, autoreleases, and matched CoreFoundation 'Get' rule ownership. | **PENDING COMMIT** |

