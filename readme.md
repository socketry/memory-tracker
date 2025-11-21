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

### v1.5.1

  - Improve performance of object table.

### v1.5.0

  - Add `Capture#each_object` for getting all retained objects.
  - Add `retained_addresses:` option to `Sampler#analyze` to capture addresses.
  - Add `Sampler#analyze(retained_minimum: 100)` - if the retained\_size is less than this, the analyse won't proceed.
  - Remove `Memory::Profiler::Graph` - it's too slow for practical use.
  - Add `Memory::Profiler.address_of(object)` to get the memory address of an object.

### v1.4.0

  - Implement [Cooper-Harvey-Kennedy](https://www.cs.tufts.edu/~nr/cs257/archive/keith-cooper/dom14.pdf) algorithm for finding root objects in memory leaks.
  - Rework capture to track objects by `object_id` exclusively.

### v1.3.0

  - **Breaking**: Renamed `Capture#count_for` to `Capture#retained_count_of` for better clarity and consistency.
  - **Breaking**: Changed `CallTree#top_paths(limit)` to `CallTree#top_paths(limit:)` - now uses keyword argument.
  - **Breaking**: Changed `CallTree#hotspots(limit)` to `CallTree#hotspots(limit:)` - now uses keyword argument.
  - Simplified `Sampler#analyze` return structure to `{allocations: {...}, call_tree: {...}}` format.
  - Added `Allocations#as_json` and `Allocations#to_json` methods for JSON serialization.
  - Added `CallTree#as_json` and `CallTree#to_json` methods for JSON serialization with configurable options.
  - `Memory::Profiler::Allocations.new` can now be instantiated directly (primarily for testing).
  - `Sampler#statistics` is now a deprecated alias for `Sampler#analyze`.
  - **Breaking**: Removed `Sampler#all_statistics` method.

### v1.2.0

  - Enable custom `depth:` and `filter:` options to `Sampler#track`.
  - Change default filter to no-op.
  - Add option to run GC with custom options before each sample to reduce noise.
  - Always report sampler statistics after each sample.

### v1.1.15

  - Ignore `freeobj` for classes that are not being tracked.

### v1.1.14

  - Ignore `freeobj` events for objects with anonymous classes that are not tracked (and thus become `T_NONE`).

### v1.1.13

  - Fix sampler loop interval handling.
  - Log capture statistics from sampler run loop.

### v1.1.12

  - Use `rb_obj_id` for tracking object states to avoid compaction issues.

### v1.1.11

  - Double buffer shared events queues to fix queue corruption.

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
