# Releases

## v1.1.10

  - Added `Capture#new_count` - returns total number of allocations tracked across all classes.
  - Added `Capture#free_count` - returns total number of objects freed across all classes.
  - Added `Capture#retained_count` - returns retained object count (new\_count - free\_count).
  - **Critical:** Fixed GC crash during compaction caused by missing write barriers in event queue.
  - Fixed allocation/deallocation counts being inaccurate when objects are allocated during callbacks or freed after compaction.
  - `Capture#clear` now raises `RuntimeError` if called while capture is running. Call `stop()` before `clear()`.

## v1.1.9

  - More write barriers...

## v1.1.8

  - Use single global queue for event handling to avoid incorrect ordering.

## v1.1.7

  - Expose `Capture#statistics` for debugging internal memory tracking state.

## v1.1.6

  - Write barriers all the things.
  - Better state handling and object increment/decrement counting.
  - Better call tree handling - including support for `prune!`.

## v1.1.5

  - Use queue for `newobj` too to avoid invoking user code during object allocation.

## v1.1.2

  - Fix handling of GC compaction (I hope).

## v0.1.0

  - Initial implementation.
