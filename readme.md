# Memory::Profiler

Efficient memory allocation tracking focused on **retained objects only**. Automatically tracks allocations and cleans up when objects are freed, giving you precise data on memory leaks.

[![Development Status](https://github.com/socketry/memory-profiler/workflows/Test/badge.svg)](https://github.com/socketry/memory-profiler/actions?workflow=Test)

## Features

  - **Retained Objects Only**: Uses `RUBY_INTERNAL_EVENT_NEWOBJ` and `RUBY_INTERNAL_EVENT_FREEOBJ` to automatically track only objects that survive GC.
  - **O(1) Live Counts**: Maintains per-class counters updated on alloc/free - no heap enumeration needed\!
  - **Tree-Based Analysis**: Deduplicates common call paths using an efficient tree structure.

## Usage

Please see the [project documentation](https://socketry.github.io/memory-profiler/) for more details.

  - [Getting Started](https://socketry.github.io/memory-profiler/guides/getting-started/index) - This guide explains how to use `memory-profiler` to automatically detect and diagnose memory leaks in Ruby applications.

  - [Rack Integration](https://socketry.github.io/memory-profiler/guides/rack-integration/index) - This guide explains how to integrate `memory-profiler` into Rack applications for automatic memory leak detection.

## Releases

Please see the [project releases](https://socketry.github.io/memory-profiler/releases/index) for all releases.

### v1.1.12

  - Use `rb_obj_id` for tracking object states to avoid compaction issues.

### v1.1.11

  - Double buffer shared events queues to fix queue corruption.

### v1.1.10

  - Added `Capture#new_count` - returns total number of allocations tracked across all classes.
  - Added `Capture#free_count` - returns total number of objects freed across all classes.
  - Added `Capture#retained_count` - returns retained object count (new\_count - free\_count).
  - **Critical:** Fixed GC crash during compaction caused by missing write barriers in event queue.
  - Fixed allocation/deallocation counts being inaccurate when objects are allocated during callbacks or freed after compaction.
  - `Capture#clear` now raises `RuntimeError` if called while capture is running. Call `stop()` before `clear()`.

### v1.1.9

  - More write barriers...

### v1.1.8

  - Use single global queue for event handling to avoid incorrect ordering.

### v1.1.7

  - Expose `Capture#statistics` for debugging internal memory tracking state.

### v1.1.6

  - Write barriers all the things.
  - Better state handling and object increment/decrement counting.
  - Better call tree handling - including support for `prune!`.

### v1.1.5

  - Use queue for `newobj` too to avoid invoking user code during object allocation.

### v1.1.2

  - Fix handling of GC compaction (I hope).

### v0.1.0

  - Initial implementation.

## Contributing

We welcome contributions to this project.

1.  Fork it.
2.  Create your feature branch (`git checkout -b my-new-feature`).
3.  Commit your changes (`git commit -am 'Add some feature'`).
4.  Push to the branch (`git push origin my-new-feature`).
5.  Create new Pull Request.

### Developer Certificate of Origin

In order to protect users of this project, we require all contributors to comply with the [Developer Certificate of Origin](https://developercertificate.org/). This ensures that all contributions are properly licensed and attributed.

### Community Guidelines

This project is best served by a collaborative and respectful environment. Treat each other professionally, respect differing viewpoints, and engage constructively. Harassment, discrimination, or harmful behavior is not tolerated. Communicate clearly, listen actively, and support one another. If any issues arise, please inform the project maintainers.
