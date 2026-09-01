# Firmware Memory Architecture

Status: working design record  
Last updated: 2026-08-11

This document tracks the firmware memory audit and the decisions made from it.
The topics are deliberately handled one at a time so that a memory reduction
does not accidentally remove useful behavior or replace deterministic storage
with a more fragile heap pattern.

## Baseline

Clean local builds currently produce the following linked internal-SRAM use:

| Target | PlatformIO data/BSS headline | Linked SRAM including IRAM code | Uncommitted before runtime |
| --- | ---: | ---: | ---: |
| X3 | 155,436 B (47.4%) | 242,155 / 321,296 B (75.4%) | 79,141 B |
| X4 | 151,012 B (46.1%) | 237,731 / 321,296 B (74.0%) | 83,565 B |

The PlatformIO headline excludes about 86.7 KB of linked IRAM code. Flash is
not presently the limiting resource. The practical risks are runtime heap
consumption and fragmentation: total free heap can look acceptable while the
largest contiguous free block is too small for BLE discovery, a decoder, or a
reader allocation.

Existing design choices worth preserving include the single framebuffer,
bounded notification/card stores, BLE/Reader radio turn-taking, complete BLE
release before Wi-Fi transfer, and lending the idle framebuffer to the EPUB
inflate operation.

## Audit topics

1. Static scene lifetimes — understood below; implementation not started.
2. Generic BLE card duplication and Block rendering copies.
3. BLE shutdown/restart ownership and callback leaks.
4. Persistence and transfer scratch-buffer duplication.
5. Reader parser ownership and failure behavior.
6. Memory modes, arenas, telemetry, and release gates.

## 1. Static scene lifetimes

### Why the scenes are static

`AppScenes.cpp` creates one global instance of every scene and passes references
to `SceneManager::switchTo()`. This pattern was introduced in the first scene
manager commit (`6abe9d1`, 2026-07-02) with the explicit rationale:

> All scenes are static instances — fixed allocation, zero heap churn.

At that time the registry contained only Launcher, About, and one small shared
Placeholder scene. For those objects, the choice was appropriate:

- scene addresses never change;
- navigation cannot fail because of allocation;
- entering a scene does not fragment the heap;
- small UI state, such as the launcher's selected tile, naturally survives a
  round trip to another scene;
- the manager has simple non-owning `Scene*`/`Scene&` semantics.

Smooth navigation is therefore a secondary benefit, but it was not the main
reason for the design. The principal goal was deterministic memory. In the
current firmware, scene construction would also be insignificant compared with
the roughly 450 ms or longer e-ink refresh. Heavy scene work such as SD scans,
BLE requests, Reader setup, and Wi-Fi startup already happens in `onEnter()` or
after the first render; a static constructor does not preload that work.

### What changed

Later features followed the same registry pattern but added large working sets
inside their scene objects. Because the objects are global, those working sets
now occupy RAM during every mode, even when their scene is inactive.

The largest examples measured in the X3 ELF are:

| Static object | Approximate size | Main inactive cost |
| --- | ---: | --- |
| Reader scene | 10,456 B | 32-entry book catalog is about 9 KB |
| File Transfer scene | 4,504 B | upload buffer is 4 KB |
| Settings scene | 1,192 B | firmware file-picker entries |

File Transfer also has a separate static 4 KB download buffer. It is not part
of the scene object, but has the same always-resident lifetime problem.

The original rule, "make the scene static so it never allocates," has therefore
mixed together two things with different desirable lifetimes:

1. **Scene controller state:** selected item, view enum, dirty flags, cached
   geometry, and references to fixed stores. This is small and safe to retain.
2. **Scene working state:** book catalogs, file listings, network buffers,
   decoder/parser state, and other bulk scratch. This is needed only while a
   particular feature is active.

### Does static allocation materially improve the user experience?

It improves reliability, and in a few places it preserves convenient UI state.
It does not materially improve transition speed on this device:

- most scenes reset selection/scroll state in `onEnter()` anyway;
- Reader explicitly tears down EPUB, section, renderer, and cover state in
  `onExit()` and reloads persisted progress on entry;
- File Transfer explicitly resets to Idle on every entry;
- Settings resets to its menu on every entry;
- the display refresh dominates any small-object setup time.

The notable state that intentionally survives is the launcher's selected tile.
That requires only a few bytes and is a good fit for a static controller.

### Provisional decision

Do **not** replace the registry with unconstrained `new`/`delete` of whole scene
objects on every navigation. That would trade known BSS use for heap churn and
would weaken the manager's simple lifetime contract.

Instead, split controller lifetime from working-data lifetime:

- Keep all small scene controller objects static.
- Keep user-visible persistent data in the existing fixed stores or NVS, not in
  a scene's temporary heap graph.
- Reader should acquire its catalog/engine working state after BLE is quiesced
  and release it before BLE resumes. Initially, reduce the 32 fully expanded
  entries to an indexed path list plus the four visible tile records.
- Settings should claim file-picker storage only while the picker is open and
  release it when leaving the picker or scene.
- File Transfer should claim one shared 4 KB transfer buffer only after BLE has
  released its memory; upload and download reuse the same buffer.
- Every acquisition must have a bounded size and a controlled failure path.

This preserves the useful original property—navigation itself does not allocate
or fail—while moving only bulk, mutually exclusive work into explicit mode-owned
storage.

### Evidence required before implementation

Before changing item 1:

1. Record `sizeof()`/ELF sizes for each scene as the regression baseline.
2. Time each scene's `onEnter()` through first composed frame on X3 hardware.
3. List the UI fields expected to survive exit/re-entry; persist or retain only
   those fields.
4. Define ownership and cleanup tests for Reader, Settings picker, and Transfer.
5. Run repeated enter/exit tests while tracking free heap and largest block.

Success means reclaiming inactive memory without adding general-purpose heap
churn to ordinary launcher, notification, Block, Today, Priorities, or Workout
navigation.

## Next topic

The next item to understand is the pair of retained `CompanionCardState`
objects and the by-value copy used by Block rendering. No change should be made
until its data flow, persistence behavior, and fallback semantics are mapped.
