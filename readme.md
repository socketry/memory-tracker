# Memory::Tracker

Efficient memory allocation tracking focused on **retained objects only**. Automatically tracks allocations and cleans up when objects are freed, giving you precise data on memory leaks.

[![Development Status](https://github.com/socketry/memory-tracker/workflows/Test/badge.svg)](https://github.com/socketry/memory-tracker/actions?workflow=Test)

## Features

  - **Retained Objects Only**: Uses `RUBY_INTERNAL_EVENT_NEWOBJ` and `RUBY_INTERNAL_EVENT_FREEOBJ` to automatically track only objects that survive GC.
  - **O(1) Live Counts**: Maintains per-class counters updated on alloc/free - no heap enumeration needed\!
  - **Tree-Based Analysis**: Deduplicates common call paths using an efficient tree structure.
  - **Native C Extension**: **Required** - uses Ruby internal events not available in pure Ruby.
  - **Configurable Depth**: Control how deep to capture call stacks.

## Usage

Please see the [project documentation](https://socketry.github.io/memory-tracker/) for more details.

  - [Getting Started](https://socketry.github.io/memory-tracker/guides/getting-started/index) - This guide explains how to use `memory-tracker` to detect and diagnose memory leaks in Ruby applications.

## Releases

Please see the [project releases](https://socketry.github.io/memory-tracker/releases/index) for all releases.

### Unreleased

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
