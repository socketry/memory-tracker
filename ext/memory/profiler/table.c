// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "table.h"
#include <stdlib.h>
#include <string.h>

// Use the Entry struct from header
// (No local definition needed)

#define INITIAL_CAPACITY 1024
#define LOAD_FACTOR 0.75

// Create a new table
struct Memory_Profiler_Object_Table* Memory_Profiler_Object_Table_new(size_t initial_capacity) {
	struct Memory_Profiler_Object_Table *table = malloc(sizeof(struct Memory_Profiler_Object_Table));
	
	if (!table) {
		return NULL;
	}
	
	table->capacity = initial_capacity > 0 ? initial_capacity : INITIAL_CAPACITY;
	table->count = 0;
	table->strong = 0;  // Start as weak table (strong == 0 means weak)
	
	// Use calloc to zero out entries (Qnil = 0)
	table->entries = calloc(table->capacity, sizeof(struct Memory_Profiler_Object_Table_Entry));
	
	if (!table->entries) {
		free(table);
		return NULL;
	}
	
	return table;
}

// Free the table
void Memory_Profiler_Object_Table_free(struct Memory_Profiler_Object_Table *table) {
	if (table) {
		free(table->entries);
		free(table);
	}
}

// Simple hash function for object addresses
static inline size_t hash_object(VALUE object, size_t capacity) {
	// Use address bits, shift right to ignore alignment
	return ((size_t)object >> 3) % capacity;
}

// Find entry index for an object (linear probing)
// Returns index if found, or index of empty slot if not found
static size_t find_entry(struct Memory_Profiler_Object_Table_Entry *entries, size_t capacity, VALUE object, int *found) {
	size_t index = hash_object(object, capacity);
	size_t start = index;
	
	*found = 0;
	
	do {
		if (entries[index].object == 0) {
			// Empty slot (calloc zeros memory)
			return index;
		}
		
		if (entries[index].object == object) {
			// Found it
			*found = 1;
			return index;
		}
		
		// Linear probe
		index = (index + 1) % capacity;
	} while (index != start);
	
	// Table is full (shouldn't happen with load factor)
	return index;
}

// Resize the table (only called from insert, not during GC)
static void resize_table(struct Memory_Profiler_Object_Table *table) {
	size_t old_capacity = table->capacity;
	struct Memory_Profiler_Object_Table_Entry *old_entries = table->entries;
	
	// Double capacity
	table->capacity = old_capacity * 2;
	table->count = 0;
	table->entries = calloc(table->capacity, sizeof(struct Memory_Profiler_Object_Table_Entry));
	
	if (!table->entries) {
		// Resize failed - restore old state
		table->capacity = old_capacity;
		table->entries = old_entries;
		return;
	}
	
	// Rehash all entries
	for (size_t i = 0; i < old_capacity; i++) {
		if (old_entries[i].object != Qnil) {
			int found;
			size_t new_index = find_entry(table->entries, table->capacity, old_entries[i].object, &found);
			table->entries[new_index] = old_entries[i];
			table->count++;
		}
	}
	
	free(old_entries);
}

// Insert object, returns pointer to entry for caller to fill
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_insert(struct Memory_Profiler_Object_Table *table, VALUE object) {
	// Resize if load factor exceeded
	if ((double)table->count / table->capacity > LOAD_FACTOR) {
		resize_table(table);
	}
	
	int found;
	size_t index = find_entry(table->entries, table->capacity, object, &found);
	
	if (!found) {
		table->count++;
		// Zero out the entry
		table->entries[index].object = object;
		table->entries[index].klass = 0;
		table->entries[index].data = 0;
		table->entries[index].allocations = 0;
	}
	
	// Set object (might be updating existing entry)
	table->entries[index].object = object;
	
	// Return pointer for caller to fill fields
	return &table->entries[index];
}

// Lookup entry for object - returns pointer or NULL
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_lookup(struct Memory_Profiler_Object_Table *table, VALUE object) {
	int found;
	size_t index = find_entry(table->entries, table->capacity, object, &found);
	
	if (found) {
		return &table->entries[index];
	}
	
	return NULL;
}

// Delete object from table
void Memory_Profiler_Object_Table_delete(struct Memory_Profiler_Object_Table *table, VALUE object) {
	int found;
	size_t index = find_entry(table->entries, table->capacity, object, &found);
	
	if (!found) {
		return;
	}
	
	// Mark as deleted (set to 0/NULL)
	table->entries[index].object = 0;
	table->entries[index].klass = 0;
	table->entries[index].data = 0;
	table->entries[index].allocations = 0;
	table->count--;
	
	// Rehash following entries to fix probe chains
	size_t next = (index + 1) % table->capacity;
	while (table->entries[next].object != 0) {
		// Save entry values
		VALUE obj = table->entries[next].object;
		VALUE k = table->entries[next].klass;
		VALUE d = table->entries[next].data;
		VALUE a = table->entries[next].allocations;
		
		// Remove this entry (set to 0/NULL)
		table->entries[next].object = 0;
		table->entries[next].klass = 0;
		table->entries[next].data = 0;
		table->entries[next].allocations = 0;
		table->count--;
		
		// Reinsert (will find correct spot and fill fields)
		struct Memory_Profiler_Object_Table_Entry *new_entry = Memory_Profiler_Object_Table_insert(table, obj);
		new_entry->klass = k;
		new_entry->data = d;
		new_entry->allocations = a;
		
		next = (next + 1) % table->capacity;
	}
}

// Mark all entries for GC
void Memory_Profiler_Object_Table_mark(struct Memory_Profiler_Object_Table *table) {
	if (!table) return;
	
	for (size_t i = 0; i < table->capacity; i++) {
		struct Memory_Profiler_Object_Table_Entry *entry = &table->entries[i];
		if (entry->object != 0) {
			// Mark object key if table is strong (strong > 0)
			// When weak (strong == 0), object keys can be GC'd (that's how we detect frees)
			if (table->strong > 0) {
				rb_gc_mark_movable(entry->object);
			}
			
			// Always mark the other fields (klass, data, allocations) - we own these
			if (entry->klass) rb_gc_mark_movable(entry->klass);
			if (entry->data) rb_gc_mark_movable(entry->data);
			if (entry->allocations) rb_gc_mark_movable(entry->allocations);
		}
	}
}

// Update object pointers during compaction
void Memory_Profiler_Object_Table_compact(struct Memory_Profiler_Object_Table *table) {
	if (!table || table->count == 0) return;
	
	// First pass: check if any objects moved
	int any_moved = 0;
	for (size_t i = 0; i < table->capacity; i++) {
		if (table->entries[i].object != 0) {
			VALUE new_loc = rb_gc_location(table->entries[i].object);
			if (new_loc != table->entries[i].object) {
				any_moved = 1;
				break;
			}
		}
	}
	
	// If nothing moved, just update VALUE fields and we're done
	if (!any_moved) {
		for (size_t i = 0; i < table->capacity; i++) {
			if (table->entries[i].object != 0) {
				// Update VALUE fields if they moved
				table->entries[i].klass = rb_gc_location(table->entries[i].klass);
				table->entries[i].data = rb_gc_location(table->entries[i].data);
				table->entries[i].allocations = rb_gc_location(table->entries[i].allocations);
			}
		}
		return;
	}
	
	// Something moved - need to rehash entire table
	// Collect all entries into temporary array (use system malloc, not Ruby's)
	struct Memory_Profiler_Object_Table_Entry *temp_entries = malloc(table->count * sizeof(struct Memory_Profiler_Object_Table_Entry));
	if (!temp_entries) {
		// Allocation failed - this is bad, but can't do much during GC
		return;
	}
	
	size_t temp_count = 0;
	for (size_t i = 0; i < table->capacity; i++) {
		if (table->entries[i].object != 0) {
			// Update all pointers first
			temp_entries[temp_count].object = rb_gc_location(table->entries[i].object);
			temp_entries[temp_count].klass = rb_gc_location(table->entries[i].klass);
			temp_entries[temp_count].data = rb_gc_location(table->entries[i].data);
			temp_entries[temp_count].allocations = rb_gc_location(table->entries[i].allocations);
			temp_count++;
		}
	}
	
	// Clear the table (zero out all entries)
	memset(table->entries, 0, table->capacity * sizeof(struct Memory_Profiler_Object_Table_Entry));
	table->count = 0;
	
	// Reinsert all entries with new hash values
	for (size_t i = 0; i < temp_count; i++) {
		int found;
		size_t index = find_entry(table->entries, table->capacity, temp_entries[i].object, &found);
		
		// Insert at new location
		table->entries[index] = temp_entries[i];
		table->count++;
	}
	
	// Free temporary array
	free(temp_entries);
}

// Delete by entry pointer (faster - avoids second lookup)
void Memory_Profiler_Object_Table_delete_entry(struct Memory_Profiler_Object_Table *table, struct Memory_Profiler_Object_Table_Entry *entry) {
	// Calculate index from pointer
	size_t index = entry - table->entries;
	
	// Validate it's within our table
	if (index >= table->capacity) {
		return;  // Invalid pointer
	}
	
	// Check if entry is actually occupied
	if (entry->object == 0) {
		return;  // Already deleted
	}
	
	// Mark as deleted (set to 0/NULL)
	entry->object = 0;
	entry->klass = 0;
	entry->data = 0;
	entry->allocations = 0;
	table->count--;
	
	// Rehash following entries to fix probe chains
	size_t next = (index + 1) % table->capacity;
	while (table->entries[next].object != 0) {
		// Save entry values
		VALUE obj = table->entries[next].object;
		VALUE k = table->entries[next].klass;
		VALUE d = table->entries[next].data;
		VALUE a = table->entries[next].allocations;
		
		// Remove this entry (set to 0/NULL)
		table->entries[next].object = 0;
		table->entries[next].klass = 0;
		table->entries[next].data = 0;
		table->entries[next].allocations = 0;
		table->count--;
		
		// Reinsert (will find correct spot and fill fields)
		struct Memory_Profiler_Object_Table_Entry *new_entry = Memory_Profiler_Object_Table_insert(table, obj);
		new_entry->klass = k;
		new_entry->data = d;
		new_entry->allocations = a;
		
		next = (next + 1) % table->capacity;
	}
}

// Get current size
size_t Memory_Profiler_Object_Table_size(struct Memory_Profiler_Object_Table *table) {
	return table->count;
}

// Increment strong reference count (makes table strong when > 0)
void Memory_Profiler_Object_Table_increment_strong(struct Memory_Profiler_Object_Table *table) {
	if (table) {
		table->strong++;
	}
}

// Decrement strong reference count (makes table weak when == 0)
void Memory_Profiler_Object_Table_decrement_strong(struct Memory_Profiler_Object_Table *table) {
	if (table && table->strong > 0) {
		table->strong--;
	}
}

