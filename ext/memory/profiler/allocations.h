// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include <ruby/st.h>

// Per-class allocation tracking record:
struct Memory_Profiler_Capture_Allocations {
	// Optional Ruby proc/lambda to call on allocation.
	VALUE callback;

	// Total allocations seen since tracking started.
	size_t new_count;
	// // Total frees seen since tracking started.
	size_t free_count;
	// Live count = new_count - free_count.
};

// Wrap an allocations record in a VALUE.
VALUE Memory_Profiler_Allocations_wrap(struct Memory_Profiler_Capture_Allocations *record);

// Get allocations record from wrapper VALUE.
struct Memory_Profiler_Capture_Allocations* Memory_Profiler_Allocations_get(VALUE self);

// Clear/reset allocation counts for a record.
void Memory_Profiler_Allocations_clear(VALUE allocations);

// Initialize the Allocations class.
void Init_Memory_Profiler_Allocations(VALUE Memory_Profiler);
