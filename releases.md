# Releases

## Unreleased

  - Add `Capture#each_object` for getting all retained objects.
  - Add `retained_addresses:` option to `Sampler#analyze` to capture addresses.
  - Add `Sampler#analyze(retained_minimum: 100)` - if the retained_size is less than this, the analyse won't proceed.
  - Remove `Memory::Profiler::Graph` - it's too slow for practical use.
  - Add `Memory::Profiler.address_of(object)` to get the memory address of an object.

## v1.4.0

  - Implement [Cooper-Harvey-Kennedy](https://www.cs.tufts.edu/~nr/cs257/archive/keith-cooper/dom14.pdf) algorithm for finding root objects in memory leaks.
  - Rework capture to track objects by `object_id` exclusively.

## v1.3.0

  - **Breaking**: Renamed `Capture#count_for` to `Capture#retained_count_of` for better clarity and consistency.
  - **Breaking**: Changed `CallTree#top_paths(limit)` to `CallTree#top_paths(limit:)` - now uses keyword argument.
  - **Breaking**: Changed `CallTree#hotspots(limit)` to `CallTree#hotspots(limit:)` - now uses keyword argument.
  - Simplified `Sampler#analyze` return structure to `{allocations: {...}, call_tree: {...}}` format.
  - Added `Allocations#as_json` and `Allocations#to_json` methods for JSON serialization.
  - Added `CallTree#as_json` and `CallTree#to_json` methods for JSON serialization with configurable options.
  - `Memory::Profiler::Allocations.new` can now be instantiated directly (primarily for testing).
  - `Sampler#statistics` is now a deprecated alias for `Sampler#analyze`.
  - **Breaking**: Removed `Sampler#all_statistics` method.

## v1.2.0

  - Enable custom `depth:` and `filter:` options to `Sampler#track`.
  - Change default filter to no-op.
  - Add option to run GC with custom options before each sample to reduce noise.
  - Always report sampler statistics after each sample.

## v1.1.15

  - Ignore `freeobj` for classes that are not being tracked.

## v1.1.14

  - Ignore `freeobj` events for objects with anonymous classes that are not tracked (and thus become `T_NONE`).

## v1.1.13

  - Fix sampler loop interval handling.
  - Log capture statistics from sampler run loop.

## v1.1.12

  - Use `rb_obj_id` for tracking object states to avoid compaction issues.

## v1.1.11

  - Double buffer shared events queues to fix queue corruption.

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
