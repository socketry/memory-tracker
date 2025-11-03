# Releases

## Unreleased

  - Write barriers all the things.
  - Better state handling and object increment/decrement counting.
  - Better call tree handling - including support for `prune!`.

## v1.1.5

  - Use queue for `newobj` too to avoid invoking user code during object allocation.

## v1.1.2

  - Fix handling of GC compaction (I hope).

## v0.1.0

  - Initial implementation.
