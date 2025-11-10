// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#pragma once

#include <ruby.h>
#include <stddef.h>

// Entry in the object table
struct Memory_Profiler_Object_Table_Entry {
	// Object pointer (key):
	VALUE object;
	// The class of the allocated object:
	VALUE klass;
	// User-defined state from callback:
	VALUE data;
	// The Allocations wrapper for this class:
	VALUE allocations;
};

// Custom object table for tracking allocations during GC.
// Uses system malloc/free (not ruby_xmalloc) to be safe during GC compaction.
// Keys are object addresses (updated during compaction).
struct Memory_Profiler_Object_Table {
	// Strong reference count: 0 = weak (don't mark keys), >0 = strong (mark keys)
	int strong;

	size_t capacity;  // Total slots
	size_t count;     // Used slots
	struct Memory_Profiler_Object_Table_Entry *entries;  // System malloc'd array
};

// Create a new object table with initial capacity
struct Memory_Profiler_Object_Table* Memory_Profiler_Object_Table_new(size_t initial_capacity);

// Free the table and all its memory
void Memory_Profiler_Object_Table_free(struct Memory_Profiler_Object_Table *table);

// Insert an object, returns pointer to entry for caller to fill fields.
// Safe to call from postponed job (not during GC).
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_insert(struct Memory_Profiler_Object_Table *table, VALUE object);

// Lookup entry for an object. Returns pointer to entry or NULL if not found.
// Safe to call during FREEOBJ event handler (no allocation) - READ ONLY!
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_lookup(struct Memory_Profiler_Object_Table *table, VALUE object);

// Delete an object. Safe to call from postponed job (not during GC).
void Memory_Profiler_Object_Table_delete(struct Memory_Profiler_Object_Table *table, VALUE object);

// Delete by entry pointer (faster - no second lookup needed).
// Safe to call from postponed job (not during GC).
// entry must be a valid pointer from Object_Table_lookup.
void Memory_Profiler_Object_Table_delete_entry(struct Memory_Profiler_Object_Table *table, struct Memory_Profiler_Object_Table_Entry *entry);

// Mark all entries for GC.
// Must be called from dmark callback.
void Memory_Profiler_Object_Table_mark(struct Memory_Profiler_Object_Table *table);

// Update object pointers after compaction.
// Must be called from dcompact callback.
void Memory_Profiler_Object_Table_compact(struct Memory_Profiler_Object_Table *table);

// Get current size
size_t Memory_Profiler_Object_Table_size(struct Memory_Profiler_Object_Table *table);

// Increment strong reference count
// When strong > 0, table is strong and will mark object keys during GC
void Memory_Profiler_Object_Table_increment_strong(struct Memory_Profiler_Object_Table *table);

// Decrement strong reference count
// When strong == 0, table is weak and will not mark object keys during GC
void Memory_Profiler_Object_Table_decrement_strong(struct Memory_Profiler_Object_Table *table);

