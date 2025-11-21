// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum {
	DEBUG = 1,
	
	// Performance monitoring thresholds

	// Log warning if probe chain exceeds this
	WARN_PROBE_LENGTH = 100,

	// Safety limit - abort search if exceeded
	MAX_PROBE_LENGTH = 10000,
};

// Use the Entry struct from header
// (No local definition needed)
const size_t INITIAL_CAPACITY = 1024;
const float LOAD_FACTOR = 0.50;  // Reduced from 0.75 to avoid clustering


VALUE TOMBSTONE = Qnil;

// Create a new table
struct Memory_Profiler_Object_Table* Memory_Profiler_Object_Table_new(size_t initial_capacity) {
	struct Memory_Profiler_Object_Table *table = malloc(sizeof(struct Memory_Profiler_Object_Table));
	
	if (!table) {
		return NULL;
	}
	
	table->capacity = initial_capacity > 0 ? initial_capacity : INITIAL_CAPACITY;
	table->count = 0;
	table->tombstones = 0;
	table->strong = 0;  // Start as weak table (strong == 0 means weak)
	
	// Use calloc to zero out entries (0 = empty slot)
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

// Hash function for object addresses
// Uses multiplicative hashing with bit mixing to reduce clustering
static inline size_t hash_object(VALUE object, size_t capacity) {
	size_t hash = (size_t)object;
	
	// Remove alignment bits (objects are typically 8-byte aligned)
	hash >>= 3;
	
	// Multiplicative hashing (Knuth's golden ratio method)
	// This helps distribute consecutive addresses across the table
	hash *= 2654435761UL;  // 2^32 / phi (golden ratio)
	
	// Mix high bits into low bits for better distribution
	hash ^= (hash >> 16);
	hash *= 0x85ebca6b;
	hash ^= (hash >> 13);
	hash *= 0xc2b2ae35;
	hash ^= (hash >> 16);
	
	return hash % capacity;
}

// Find entry index for an object (linear probing)
// Returns index if found, or index of empty slot if not found
// If table is provided (not NULL), logs statistics when probe length is excessive
static size_t find_entry(struct Memory_Profiler_Object_Table_Entry *entries, size_t capacity, VALUE object, int *found, struct Memory_Profiler_Object_Table *table, const char *operation) {
	size_t index = hash_object(object, capacity);
	size_t start = index;
	size_t probe_count = 0;
	
	*found = 0;
	
	do {
		probe_count++;
		
		// Safety check - prevent infinite loops
		if (probe_count > MAX_PROBE_LENGTH) {
			if (DEBUG && table) {
				double load = (double)table->count / capacity;
				double tomb_ratio = (double)table->tombstones / capacity;
				fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"critical\",\"operation\":\"%s\",\"event\":\"max_probes_exceeded\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
					operation, probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
			} else if (DEBUG) {
				fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"critical\",\"operation\":\"%s\",\"event\":\"max_probes_exceeded\",\"probe_count\":%zu,\"capacity\":%zu}\n", 
					operation, probe_count, capacity);
			}
			return index;
		}
		
		// Log warning for excessive probing
		if (DEBUG && probe_count == WARN_PROBE_LENGTH && table) {
			double load = (double)table->count / capacity;
			double tomb_ratio = (double)table->tombstones / capacity;
			fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"warning\",\"operation\":\"%s\",\"event\":\"long_probe_chain\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
				operation, probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
		}
		
		if (entries[index].object == 0) {
			return index;
		}
		
		if (entries[index].object != TOMBSTONE && entries[index].object == object) {
			*found = 1;
			return index;
		}
		
		index = (index + 1) % capacity;
	} while (index != start);
	
	// Table is full
	if (DEBUG && table) {
		double load = (double)table->count / capacity;
		double tomb_ratio = (double)table->tombstones / capacity;
		fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"error\",\"operation\":\"%s\",\"event\":\"table_full\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
			operation, probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
	}
	return index;
}

// Find slot for inserting an object (linear probing)
// Returns index to insert at - reuses tombstone slots if found
// If object exists, returns its index with found=1
static size_t find_insert_slot(struct Memory_Profiler_Object_Table *table, VALUE object, int *found) {
	struct Memory_Profiler_Object_Table_Entry *entries = table->entries;
	size_t capacity = table->capacity;
	size_t index = hash_object(object, capacity);
	size_t start = index;
	size_t first_tombstone = SIZE_MAX;  // Track first tombstone we encounter
	size_t probe_count = 0;
	
	*found = 0;
	
	do {
		probe_count++;
		
		// Safety check - prevent infinite loops
		if (probe_count > MAX_PROBE_LENGTH) {
			if (DEBUG) {
				double load = (double)table->count / capacity;
				double tomb_ratio = (double)table->tombstones / capacity;
				fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"critical\",\"operation\":\"insert\",\"event\":\"max_probes_exceeded\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
					probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
			}
			// Return tombstone if we found one, otherwise current position
			return (first_tombstone != SIZE_MAX) ? first_tombstone : index;
		}
		
		// Log warning for excessive probing
		if (DEBUG && probe_count == WARN_PROBE_LENGTH) {
			double load = (double)table->count / capacity;
			double tomb_ratio = (double)table->tombstones / capacity;
			fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"warning\",\"operation\":\"insert\",\"event\":\"long_probe_chain\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
				probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
		}
		
		if (entries[index].object == 0) {
			// Empty slot - use tombstone if we found one, otherwise this slot
			return (first_tombstone != SIZE_MAX) ? first_tombstone : index;
		}
		
		if (entries[index].object == TOMBSTONE) {
			// Remember first tombstone (but keep searching for existing object)
			if (first_tombstone == SIZE_MAX) {
				first_tombstone = index;
			}
		} else if (entries[index].object == object) {
			// Found existing entry
			*found = 1;
			return index;
		}
		
		index = (index + 1) % capacity;
	} while (index != start);
	
	// Table is full
	if (DEBUG) {
		double load = (double)table->count / capacity;
		double tomb_ratio = (double)table->tombstones / capacity;
		fprintf(stderr, "{\"subject\":\"Memory::Profiler::ObjectTable\",\"level\":\"error\",\"operation\":\"insert\",\"event\":\"table_full\",\"probe_count\":%zu,\"capacity\":%zu,\"count\":%zu,\"tombstones\":%zu,\"load_factor\":%.3f,\"tombstone_ratio\":%.3f}\n", 
			probe_count, capacity, table->count, table->tombstones, load, tomb_ratio);
	}
	// Use tombstone slot if we found one
	return (first_tombstone != SIZE_MAX) ? first_tombstone : index;
}

// Resize the table (only called from insert, not during GC)
// This clears all tombstones
static void resize_table(struct Memory_Profiler_Object_Table *table) {
	size_t old_capacity = table->capacity;
	struct Memory_Profiler_Object_Table_Entry *old_entries = table->entries;
	
	// Double capacity
	table->capacity = old_capacity * 2;
	table->count = 0;
	table->tombstones = 0;  // Reset tombstones
	table->entries = calloc(table->capacity, sizeof(struct Memory_Profiler_Object_Table_Entry));
	
	if (!table->entries) {
		// Resize failed - restore old state
		table->capacity = old_capacity;
		table->entries = old_entries;
		return;
	}
	
	// Rehash all non-tombstone entries
	for (size_t i = 0; i < old_capacity; i++) {
		// Skip empty slots and tombstones
		if (old_entries[i].object != 0 && old_entries[i].object != TOMBSTONE) {
			int found;
			size_t new_index = find_entry(table->entries, table->capacity, old_entries[i].object, &found, NULL, "resize");
			table->entries[new_index] = old_entries[i];
			table->count++;
		}
	}
	
	free(old_entries);
}

// Insert object, returns pointer to entry for caller to fill
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_insert(struct Memory_Profiler_Object_Table *table, VALUE object) {
	// Resize if load factor exceeded (count + tombstones)
	// This clears tombstones and gives us fresh space
	if ((double)(table->count + table->tombstones) / table->capacity > LOAD_FACTOR) {
		resize_table(table);
	}
	
	int found;
	size_t index = find_insert_slot(table, object, &found);
	
	if (!found) {
		// New entry - check if we're reusing a tombstone slot
		if (table->entries[index].object == TOMBSTONE) {
			table->tombstones--;  // Reusing tombstone
		}
		table->count++;
		// Zero out the entry
		table->entries[index].object = object;
		table->entries[index].klass = 0;
		table->entries[index].data = 0;
	} else {
		// Updating existing entry
		table->entries[index].object = object;
	}
	
	// Return pointer for caller to fill fields
	return &table->entries[index];
}

// Lookup entry for object - returns pointer or NULL
struct Memory_Profiler_Object_Table_Entry* Memory_Profiler_Object_Table_lookup(struct Memory_Profiler_Object_Table *table, VALUE object) {
	int found;
	size_t index = find_entry(table->entries, table->capacity, object, &found, table, "lookup");
	
	if (found) {
		return &table->entries[index];
	}
	
	return NULL;
}

// Delete object from table
void Memory_Profiler_Object_Table_delete(struct Memory_Profiler_Object_Table *table, VALUE object) {
	int found;
	size_t index = find_entry(table->entries, table->capacity, object, &found, table, "delete");
	
	if (!found) {
		return;
	}
	
	// Mark as tombstone - no rehashing needed!
	table->entries[index].object = TOMBSTONE;
	table->entries[index].klass = 0;
	table->entries[index].data = 0;
	table->count--;
	table->tombstones++;
}

// Mark all entries for GC
void Memory_Profiler_Object_Table_mark(struct Memory_Profiler_Object_Table *table) {
	if (!table) return;
	
	for (size_t i = 0; i < table->capacity; i++) {
		struct Memory_Profiler_Object_Table_Entry *entry = &table->entries[i];
		// Skip empty slots and tombstones
		if (entry->object != 0 && entry->object != TOMBSTONE) {
			// Mark object key if table is strong (strong > 0)
			// When weak (strong == 0), object keys can be GC'd (that's how we detect frees)
			if (table->strong > 0) {
				rb_gc_mark_movable(entry->object);
			}
			
			// Always mark the other fields (klass, data) - we own these
			if (entry->klass) rb_gc_mark_movable(entry->klass);
			if (entry->data) rb_gc_mark_movable(entry->data);
		}
	}
}

// Update object pointers during compaction
void Memory_Profiler_Object_Table_compact(struct Memory_Profiler_Object_Table *table) {
	if (!table || table->count == 0) return;
	
	// First pass: check if any objects moved
	int any_moved = 0;
	for (size_t i = 0; i < table->capacity; i++) {
		// Skip empty slots and tombstones
		if (table->entries[i].object != 0 && table->entries[i].object != TOMBSTONE) {
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
			// Skip empty slots and tombstones
			if (table->entries[i].object != 0 && table->entries[i].object != TOMBSTONE) {
				// Update VALUE fields if they moved
				table->entries[i].klass = rb_gc_location(table->entries[i].klass);
				table->entries[i].data = rb_gc_location(table->entries[i].data);
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
		// Skip empty slots and tombstones
		if (table->entries[i].object != 0 && table->entries[i].object != TOMBSTONE) {
			// Update all pointers first
			temp_entries[temp_count].object = rb_gc_location(table->entries[i].object);
			temp_entries[temp_count].klass = rb_gc_location(table->entries[i].klass);
			temp_entries[temp_count].data = rb_gc_location(table->entries[i].data);
			temp_count++;
		}
	}
	
	// Clear the table (zero out all entries, clears tombstones too)
	memset(table->entries, 0, table->capacity * sizeof(struct Memory_Profiler_Object_Table_Entry));
	table->count = 0;
	table->tombstones = 0;  // Compaction clears tombstones
	
	// Reinsert all entries with new hash values
	for (size_t i = 0; i < temp_count; i++) {
		int found;
		size_t index = find_entry(table->entries, table->capacity, temp_entries[i].object, &found, NULL, "compact");
		
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
	
	// Check if entry is actually occupied (not empty or tombstone)
	if (entry->object == 0 || entry->object == TOMBSTONE) {
		return;  // Already deleted or empty
	}
	
	// Mark as tombstone - no rehashing needed!
	entry->object = TOMBSTONE;
	entry->klass = 0;
	entry->data = 0;
	table->count--;
	table->tombstones++;
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

