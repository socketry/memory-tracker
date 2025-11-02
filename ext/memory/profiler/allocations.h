// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include "ruby.h"
#include "ruby/st.h"

// Per-class allocation tracking record
struct Memory_Profiler_Capture_Allocations {
	VALUE callback;   // Optional Ruby proc/lambda to call on allocation
	size_t new_count;  // Total allocations seen since tracking started
	size_t free_count; // Total frees seen since tracking started
	// Live count = new_count - free_count
	
	// For detailed tracking: map object (VALUE) => state (VALUE)
	// State is returned from callback on :newobj and passed back on :freeobj
	st_table *object_states;
};

// Wrap an allocations record in a VALUE
VALUE Memory_Profiler_Allocations_wrap(struct Memory_Profiler_Capture_Allocations *record);

// Get allocations record from wrapper VALUE
struct Memory_Profiler_Capture_Allocations* Memory_Profiler_Allocations_get(VALUE self);

// Clear/reset allocation counts and state for a record
void Memory_Profiler_Allocations_clear(VALUE allocations);

// Initialize the Allocations class
void Init_Memory_Profiler_Allocations(VALUE Memory_Profiler);
