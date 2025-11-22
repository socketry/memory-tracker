// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "capture.h"
#include "allocations.h"
#include "events.h"
#include "table.h"

#include <ruby/debug.h>
#include <ruby/st.h>
#include <stdatomic.h>
#include <stdio.h>

enum {
	DEBUG = 0,
};

static VALUE Memory_Profiler_Capture = Qnil;

// Event symbols:
static VALUE sym_newobj, sym_freeobj;

// Main capture state (per-instance).
struct Memory_Profiler_Capture {
	// Master switch - is tracking active? (set by start/stop).
	int running;
	
	// Should we queue callbacks? (temporarily disabled during queue processing).
	int paused;

	// Tracked classes: class => VALUE (wrapped Memory_Profiler_Capture_Allocations).
	st_table *tracked;
	
	// Custom object table: object (address) => state hash
	// Uses system malloc (GC-safe), updates addresses during compaction
	struct Memory_Profiler_Object_Table *states;

	// Total number of allocations and frees seen since tracking started.
	size_t new_count;
	size_t free_count;
};

// GC mark callback for tracked table.
static int Memory_Profiler_Capture_tracked_mark(st_data_t key, st_data_t value, st_data_t arg) {
	// Mark class as un-movable:
	// - We don't want to re-index the table if the class moves.
	// - We don't want objects in `freeobj` to have invalid class pointers (maybe helps).
	VALUE klass = (VALUE)key;
	rb_gc_mark(klass);
	
	// Mark the wrapped Allocations VALUE:
	VALUE allocations = (VALUE)value;
	rb_gc_mark_movable(allocations);
	
	return ST_CONTINUE;
}

static void Memory_Profiler_Capture_mark(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
	
	if (capture->tracked) {
		st_foreach(capture->tracked, Memory_Profiler_Capture_tracked_mark, 0);
	}
	
	Memory_Profiler_Object_Table_mark(capture->states);
}

static void Memory_Profiler_Capture_free(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
		
	if (capture->tracked) {
		st_free_table(capture->tracked);
	}
	
	if (capture->states) {
		Memory_Profiler_Object_Table_free(capture->states);
	}
	
	xfree(capture);
}

static size_t Memory_Profiler_Capture_memsize(const void *ptr) {
	const struct Memory_Profiler_Capture *capture = ptr;
	size_t size = sizeof(struct Memory_Profiler_Capture);
	
	if (capture->tracked) {
		size += capture->tracked->num_entries * (sizeof(st_data_t) + sizeof(struct Memory_Profiler_Capture_Allocations));
	}
	
	return size;
}

// Foreach callback for st_foreach_with_replace (iteration logic).
static int Memory_Profiler_Capture_tracked_foreach(st_data_t key, st_data_t value, st_data_t argp, int error) {
	return ST_REPLACE;
}

// Replace callback for st_foreach_with_replace (update logic).
static int Memory_Profiler_Capture_tracked_update(st_data_t *key, st_data_t *value, st_data_t argp, int existing) {
	// Update wrapped Allocations VALUE if it moved:
	VALUE old_allocations = (VALUE)*value;
	VALUE new_allocations = rb_gc_location(old_allocations);
	if (old_allocations != new_allocations) {
		*value = (st_data_t)new_allocations;
	}
	
	return ST_CONTINUE;
}

static void Memory_Profiler_Capture_compact(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
	
	// Update tracked keys and allocations values in-place:
	if (capture->tracked && capture->tracked->num_entries > 0) {
		if (st_foreach_with_replace(capture->tracked, Memory_Profiler_Capture_tracked_foreach, Memory_Profiler_Capture_tracked_update, 0)) {
			rb_raise(rb_eRuntimeError, "tracked modified during GC compaction");
		}
	}
	
	// Update custom object table (system malloc, safe during GC)
	if (capture->states) {
		Memory_Profiler_Object_Table_compact(capture->states);
	}
}

static const rb_data_type_t Memory_Profiler_Capture_type = {
	"Memory::Profiler::Capture",
	{
		.dmark = Memory_Profiler_Capture_mark,
		.dcompact = Memory_Profiler_Capture_compact,
		.dfree = Memory_Profiler_Capture_free,
		.dsize = Memory_Profiler_Capture_memsize,
	},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

const char *event_flag_name(rb_event_flag_t event_flag) {
	switch (event_flag) {
		case RUBY_EVENT_CALL: return "call";
		case RUBY_EVENT_C_CALL: return "c-call";
		case RUBY_EVENT_B_CALL: return "b-call";
		case RUBY_EVENT_RETURN: return "return";
		case RUBY_EVENT_C_RETURN: return "c-return";
		case RUBY_EVENT_B_RETURN: return "b-return";
		case RUBY_INTERNAL_EVENT_NEWOBJ: return "newobj";
		case RUBY_INTERNAL_EVENT_FREEOBJ: return "freeobj";
		case RUBY_INTERNAL_EVENT_GC_START: return "gc-start";
		case RUBY_INTERNAL_EVENT_GC_END_MARK: return "gc-end-mark";
		case RUBY_INTERNAL_EVENT_GC_END_SWEEP: return "gc-end-sweep";
		case RUBY_EVENT_LINE: return "line";
		default: return "unknown";
	}
}

// Process a NEWOBJ event. All allocation tracking logic is here.
// object_id parameter is the Integer object_id, NOT the raw object.
// Process a NEWOBJ event. All allocation tracking logic is here.
// object parameter is the actual object being allocated.
static void Memory_Profiler_Capture_process_newobj(VALUE self, VALUE klass, VALUE object) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// Pause the capture to prevent infinite loop:
	capture->paused += 1;
	
	// Increment global new count:
	capture->new_count++;
	
	// Look up or create allocations record for this class:
	st_data_t allocations_data;
	VALUE allocations;
	struct Memory_Profiler_Capture_Allocations *record;
	
	if (st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
		// Existing record
		allocations = (VALUE)allocations_data;
		record = Memory_Profiler_Allocations_get(allocations);
		record->new_count++;
	} else {
		// First time seeing this class, create record automatically
		record = ALLOC(struct Memory_Profiler_Capture_Allocations);
		record->callback = Qnil;
		record->new_count = 1;
		record->free_count = 0;
		
		allocations = Memory_Profiler_Allocations_wrap(record);
		st_insert(capture->tracked, (st_data_t)klass, (st_data_t)allocations);
		RB_OBJ_WRITTEN(self, Qnil, klass);
		RB_OBJ_WRITTEN(self, Qnil, allocations);
	}
	
	VALUE data = Qnil;
	if (!NIL_P(record->callback)) {
		data = rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_newobj, Qnil);
	}
	
	struct Memory_Profiler_Object_Table_Entry *entry = Memory_Profiler_Object_Table_insert(capture->states, object);
	RB_OBJ_WRITTEN(self, Qnil, object);
	RB_OBJ_WRITE(self, &entry->klass, klass);
	RB_OBJ_WRITE(self, &entry->data, data);
	
	if (DEBUG) fprintf(stderr, "[NEWOBJ] Object inserted into table: %p\n", (void*)object);
	
	// Resume the capture:
	capture->paused -= 1;
}

// Process a FREEOBJ event. All deallocation tracking logic is here.
// freeobj_data parameter is [state_hash, object] array from event handler.
static void Memory_Profiler_Capture_process_freeobj(VALUE capture_value, VALUE unused_klass, VALUE object) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(capture_value, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// Pause the capture to prevent infinite loop:
	capture->paused += 1;
	
	struct Memory_Profiler_Object_Table_Entry *entry = Memory_Profiler_Object_Table_lookup(capture->states, object);
	
	if (!entry) {
		if (DEBUG) fprintf(stderr, "[FREEOBJ] Object not found in table: %p\n", (void*)object);
		goto done;
	} else {
		if (DEBUG) fprintf(stderr, "[FREEOBJ] Object found in table: %p\n", (void*)object);
	}
	
	VALUE klass = entry->klass;
	VALUE data = entry->data;
	
	// Look up allocations from tracked table:
	st_data_t allocations_data;
	if (!st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
		// Class not tracked - shouldn't happen, but be defensive:
		if (DEBUG) fprintf(stderr, "[FREEOBJ] Class not found in tracked: %p\n", (void*)klass);
		goto done;
	}
	VALUE allocations = (VALUE)allocations_data;
	
	// Delete by entry pointer (faster - no second lookup!)
	Memory_Profiler_Object_Table_delete_entry(capture->states, entry);
	
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
	
	// Increment global free count
	capture->free_count++;
	
	// Increment per-class free count
	record->free_count++;
	
	// Call callback if present
	if (!NIL_P(record->callback) && !NIL_P(data)) {
		rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_freeobj, data);
	}

done:
	// Resume the capture:
	capture->paused -= 1;
}

// Process a single event (NEWOBJ or FREEOBJ). Called from events.c via rb_protect to catch exceptions.
void Memory_Profiler_Capture_process_event(struct Memory_Profiler_Event *event) {
	switch (event->type) {
		case MEMORY_PROFILER_EVENT_TYPE_NEWOBJ:
			Memory_Profiler_Capture_process_newobj(event->capture, event->klass, event->object);
			break;
		case MEMORY_PROFILER_EVENT_TYPE_FREEOBJ:
			Memory_Profiler_Capture_process_freeobj(event->capture, event->klass, event->object);
			break;
		default:
			// Ignore.
			break;
	}
}

#pragma mark - Event Handlers

// Check if object type is trackable. Excludes internal types (T_IMEMO, T_NODE, T_ICLASS, etc.) that don't have normal classes.
int Memory_Profiler_Capture_trackable_p(VALUE object) {
	switch (rb_type(object)) {
		// Normal Ruby objects with valid class pointers
		case RUBY_T_OBJECT:   // Plain objects
		case RUBY_T_CLASS:    // Class objects
		case RUBY_T_MODULE:   // Module objects
		case RUBY_T_STRING:   // Strings
		case RUBY_T_ARRAY:    // Arrays
		case RUBY_T_HASH:     // Hashes
		case RUBY_T_STRUCT:   // Structs
		case RUBY_T_BIGNUM:   // Big integers
		case RUBY_T_FLOAT:    // Floating point numbers
		case RUBY_T_FILE:     // File objects
		case RUBY_T_DATA:     // C extension data
		case RUBY_T_MATCH:    // Regex match data
		case RUBY_T_COMPLEX:  // Complex numbers
		case RUBY_T_RATIONAL: // Rational numbers
		case RUBY_T_REGEXP:   // Regular expressions
			return true;
		
		// Skip internal types (don't have normal class pointers):
		// - T_IMEMO: Internal memory objects (use class field for other data)
		// - T_NODE: AST nodes (internal)
		// - T_ICLASS: Include wrappers (internal)
		// - T_ZOMBIE: Finalizing objects (internal)
		// - T_MOVED: Compaction placeholders (internal)
		// - T_NONE: Uninitialized slots
		// - T_UNDEF: Undefined value marker
		default:
			return false;
	}
}

// Event hook callback with RAW_ARG
// Signature: (VALUE data, rb_trace_arg_t *trace_arg)
static void Memory_Profiler_Capture_event_callback(VALUE self, void *ptr) {
	rb_trace_arg_t *trace_arg = (rb_trace_arg_t *)ptr;
	
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE object = rb_tracearg_object(trace_arg);
	
	// We don't want to track internal non-Object allocations:
	if (!Memory_Profiler_Capture_trackable_p(object)) return;
	
	rb_event_flag_t event_flag = rb_tracearg_event_flag(trace_arg);
	
	if (event_flag == RUBY_INTERNAL_EVENT_NEWOBJ) {
		// Skip NEWOBJ if disabled (during callback) to prevent infinite recursion
		if (capture->paused) return;
		
		VALUE klass = rb_obj_class(object);
		
		// Skip if klass is not a Class
		if (rb_type(klass) != RUBY_T_CLASS) return;
		
		// Enqueue actual object (not object_id) - queue retains it until processed
		// Ruby 3.5 compatible: no need for FL_SEEN_OBJ_ID or rb_obj_id
		if (DEBUG) fprintf(stderr, "[NEWOBJ] Enqueuing event for object: %p\n", (void*)object);
		Memory_Profiler_Events_enqueue(MEMORY_PROFILER_EVENT_TYPE_NEWOBJ, self, klass, object);
	} else if (event_flag == RUBY_INTERNAL_EVENT_FREEOBJ) {
		if (DEBUG) fprintf(stderr, "[FREEOBJ] Enqueuing event for object: %p\n", (void*)object);
		Memory_Profiler_Events_enqueue(MEMORY_PROFILER_EVENT_TYPE_FREEOBJ, self, Qnil, object);
	}
}

// Allocate new capture
static VALUE Memory_Profiler_Capture_alloc(VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	VALUE obj = TypedData_Make_Struct(klass, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture) {
		rb_raise(rb_eRuntimeError, "Failed to allocate Memory::Profiler::Capture");
	}
	
	capture->tracked = st_init_numtable();
	
	if (!capture->tracked) {
		rb_raise(rb_eRuntimeError, "Failed to initialize tracked hash table");
	}
	
	// Initialize custom object table (uses system malloc, GC-safe)
	capture->states = Memory_Profiler_Object_Table_new(1024);
	if (!capture->states) {
		st_free_table(capture->tracked);
		rb_raise(rb_eRuntimeError, "Failed to initialize object table");
	}
	
	// Initialize allocation tracking counters
	capture->new_count = 0;
	capture->free_count = 0;
	
	// Initialize state flags - not running, callbacks disabled
	capture->running = 0;
	capture->paused = 0;
	
	// Global event queue system will auto-initialize on first use (lazy initialization)
	
	return obj;
}

// Initialize capture
static VALUE Memory_Profiler_Capture_initialize(VALUE self) {
	return self;
}

// Start capturing allocations
static VALUE Memory_Profiler_Capture_start(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (capture->running) return Qfalse;
	
	// Ensure global event queue system is initialized:
	// It could fail and we want to raise an error if it does, here specifically.
	Memory_Profiler_Events_instance();
	
	// Add event hook for NEWOBJ and FREEOBJ with RAW_ARG to get trace_arg
	rb_add_event_hook2(
		(rb_event_hook_func_t)Memory_Profiler_Capture_event_callback,
		RUBY_INTERNAL_EVENT_NEWOBJ | RUBY_INTERNAL_EVENT_FREEOBJ,
		self,
		RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG
	);
	
	// Set both flags - we're now running and callbacks are enabled
	capture->running = 1;
	capture->paused = 0;
	
	return Qtrue;
}

// Stop capturing allocations
static VALUE Memory_Profiler_Capture_stop(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture->running) return Qfalse;
	
	// Remove event hook using same data (self) we registered with. No more events will be queued after this point:
	rb_remove_event_hook_with_data((rb_event_hook_func_t)Memory_Profiler_Capture_event_callback, self);
	
	// Flush any pending queued events in the global queue before stopping.
	// This ensures all callbacks are invoked and object_states is properly maintained.
	Memory_Profiler_Events_process_all();
	
	// Clear both flags - we're no longer running and callbacks are disabled
	capture->running = 0;
	capture->paused = 0;
	
	return Qtrue;
}

// Add a class to track with optional callback
// Usage: track(klass) or track(klass) { |obj, klass| ... }
// Callback can call caller_locations with desired depth
// Returns the Allocations object for the tracked class
static VALUE Memory_Profiler_Capture_track(int argc, VALUE *argv, VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE klass, callback;
	rb_scan_args(argc, argv, "1&", &klass, &callback);
		
	st_data_t allocations_data;
	VALUE allocations;
	
	if (st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
		allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		RB_OBJ_WRITE(self, &record->callback, callback);
	} else {
		struct Memory_Profiler_Capture_Allocations *record = ALLOC(struct Memory_Profiler_Capture_Allocations);
		RB_OBJ_WRITE(self, &record->callback, callback);
		record->new_count = 0;
		record->free_count = 0;
		// NOTE: States table removed - now at Capture level
		
		// Wrap the record in a VALUE
		allocations = Memory_Profiler_Allocations_wrap(record);
		
		st_insert(capture->tracked, (st_data_t)klass, (st_data_t)allocations);
		RB_OBJ_WRITTEN(self, Qnil, klass);
		RB_OBJ_WRITTEN(self, Qnil, allocations);
	}
	
	return allocations;
}

// Stop tracking a class
static VALUE Memory_Profiler_Capture_untrack(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_delete(capture->tracked, (st_data_t *)&klass, &allocations_data)) {
		// The wrapped Allocations VALUE will be GC'd naturally
		// No manual cleanup needed
	}
	
	return self;
}

// Check if tracking a class
static VALUE Memory_Profiler_Capture_tracking_p(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	return st_lookup(capture->tracked, (st_data_t)klass, NULL) ? Qtrue : Qfalse;
}

// Get count of live objects for a specific class (O(1) lookup!)
static VALUE Memory_Profiler_Capture_retained_count_of(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		if (record->free_count <= record->new_count) {
			return SIZET2NUM(record->new_count - record->free_count);
		}
	}
	
	return INT2FIX(0);
}

// Iterator to reset each class record
static int Memory_Profiler_Capture_tracked_clear(st_data_t key, st_data_t value, st_data_t arg) {
	VALUE allocations = (VALUE)value;
	
	Memory_Profiler_Allocations_clear(allocations);
	
	return ST_CONTINUE;
}

// Clear all allocation tracking (resets all counts to 0)
static VALUE Memory_Profiler_Capture_clear(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// SAFETY: Don't allow clearing while running - there may be:
	// - Event handlers firing (adding to object_allocations).
	// - Events queued that haven't been processed yet.
	// - Processors trying to access the states table.
	if (capture->running) {
		rb_raise(rb_eRuntimeError, "Cannot clear while capture is running - call stop() first!");
	}
	
	// Reset all counts to 0 (don't free, just reset):
	st_foreach(capture->tracked, Memory_Profiler_Capture_tracked_clear, 0);
	
	// Clear custom object table by recreating it
	if (capture->states) {
		Memory_Profiler_Object_Table_free(capture->states);
		capture->states = Memory_Profiler_Object_Table_new(1024);
	}
	
	// Reset allocation tracking counters
	capture->new_count = 0;
	capture->free_count = 0;
	
	return self;
}

// Iterator callback for each
static int Memory_Profiler_Capture_each_allocation(st_data_t key, st_data_t value, st_data_t arg) {
	VALUE klass = (VALUE)key;
	VALUE allocations = (VALUE)value;  // Already a wrapped VALUE
	
	// Yield class and allocations wrapper
	rb_yield_values(2, klass, allocations);
	
	return ST_CONTINUE;
}

// Iterate over all tracked classes with their allocation data
static VALUE Memory_Profiler_Capture_each(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	RETURN_ENUMERATOR(self, 0, 0);
	
	st_foreach(capture->tracked, Memory_Profiler_Capture_each_allocation, 0);
	
	return self;
}

// Struct for filtering states during each_object iteration
struct Memory_Profiler_Each_Object_Arguments {
	VALUE self;
	
	// The allocations wrapper to filter by (Qnil = no filter).
	VALUE allocations;
};

// Cleanup function to re-enable GC
static VALUE Memory_Profiler_Capture_each_object_ensure(VALUE arg) {
	// Re-enable GC (rb_gc_enable returns previous state, but we don't need it)
	rb_gc_enable();
	
	return Qnil;
}

// Main iteration function
static VALUE Memory_Profiler_Capture_each_object_body(VALUE arg) {
	struct Memory_Profiler_Each_Object_Arguments *arguments = (struct Memory_Profiler_Each_Object_Arguments *)arg;
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(arguments->self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// Iterate custom object table entries
	if (capture->states) {
		if (DEBUG) fprintf(stderr, "[ITER] Iterating table, capacity=%zu, count=%zu\n", capture->states->capacity, capture->states->count);
		
		for (size_t i = 0; i < capture->states->capacity; i++) {
			struct Memory_Profiler_Object_Table_Entry *entry = &capture->states->entries[i];
			
			// Skip empty or deleted slots (0 = not set)
			if (entry->object == 0) {
				continue;
			}
			
			// Look up allocations from klass
			st_data_t allocations_data;
			VALUE allocations = Qnil;
			if (st_lookup(capture->tracked, (st_data_t)entry->klass, &allocations_data)) {
				allocations = (VALUE)allocations_data;
			}
			
			// Filter by allocations if specified
			if (!NIL_P(arguments->allocations)) {
				if (allocations != arguments->allocations) continue;
			}
			
			rb_yield_values(2, entry->object, allocations);
		}
	}
	
	return arguments->self;
}

// Iterate over tracked object IDs, optionally filtered by class
// Called as: 
//   capture.each_object(String) { |object_id, state| ... }  # Specific class
//   capture.each_object { |object_id, state| ... }          // All objects
// 
// Yields object_id as Integer. Caller can:
//   - Format as hex: "0x%x" % object_id
//   - Convert to object with ObjectSpace._id2ref (may raise RangeError if recycled)
// Future-proof for Ruby 3.5 where _id2ref is deprecated
static VALUE Memory_Profiler_Capture_each_object(int argc, VALUE *argv, VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE klass;
	rb_scan_args(argc, argv, "01", &klass);
	
	RETURN_ENUMERATOR(self, argc, argv);
	
	// Disable GC to prevent objects from being collected during iteration
	rb_gc_disable();
	
	// Process all pending events to clean up stale entries
	// At this point, all remaining objects in the table should be valid
	Memory_Profiler_Events_process_all();
	
	// If class provided, look up its allocations wrapper
	VALUE allocations = Qnil;
	if (!NIL_P(klass)) {
		st_data_t allocations_data;
		if (st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
			allocations = (VALUE)allocations_data;
		} else {
			// Class not tracked - nothing to iterate
			// Re-enable GC before returning
			rb_gc_enable();
			return self;
		}
	}
	
	// Setup arguments for iteration
	struct Memory_Profiler_Each_Object_Arguments arguments = {
		.self = self,
		.allocations = allocations
	};
	
	// Use rb_ensure to guarantee cleanup even if exception is raised
	return rb_ensure(
		Memory_Profiler_Capture_each_object_body, (VALUE)&arguments,
		Memory_Profiler_Capture_each_object_ensure, (VALUE)&arguments
	);
}

// Get allocations for a specific class
static VALUE Memory_Profiler_Capture_aref(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_lookup(capture->tracked, (st_data_t)klass, &allocations_data)) {
		return (VALUE)allocations_data;
	}
	
	return Qnil;
}

// Struct to accumulate statistics during iteration
struct Memory_Profiler_Allocations_Statistics {
	size_t total_tracked_objects;
	VALUE per_class_counts;
};

// Get internal statistics for debugging
// Returns hash with internal state sizes
static VALUE Memory_Profiler_Capture_statistics(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE statistics = rb_hash_new();
	
	// Tracked classes count
	rb_hash_aset(statistics, ID2SYM(rb_intern("tracked_count")), SIZET2NUM(capture->tracked->num_entries));
	
	// Custom object table size
	size_t states_size = capture->states ? Memory_Profiler_Object_Table_size(capture->states) : 0;
	rb_hash_aset(statistics, ID2SYM(rb_intern("object_table_size")), SIZET2NUM(states_size));
	
	return statistics;
}

// Get total new count across all classes
static VALUE Memory_Profiler_Capture_new_count(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	return SIZET2NUM(capture->new_count);
}

// Get total free count across all classes
static VALUE Memory_Profiler_Capture_free_count(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	return SIZET2NUM(capture->free_count);
}

// Get total retained count (new - free) across all classes
static VALUE Memory_Profiler_Capture_retained_count(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// Handle underflow if free_count > new_count (shouldn't happen but be safe)
	size_t retained = capture->free_count > capture->new_count ? 0 : capture->new_count - capture->free_count;
	return SIZET2NUM(retained);
}

void Init_Memory_Profiler_Capture(VALUE Memory_Profiler)
{
	// Initialize event symbols
	sym_newobj = ID2SYM(rb_intern("newobj"));
	sym_freeobj = ID2SYM(rb_intern("freeobj"));
	rb_gc_register_mark_object(sym_newobj);
	rb_gc_register_mark_object(sym_freeobj);
	
	Memory_Profiler_Capture = rb_define_class_under(Memory_Profiler, "Capture", rb_cObject);
	rb_define_alloc_func(Memory_Profiler_Capture, Memory_Profiler_Capture_alloc);
	
	rb_define_method(Memory_Profiler_Capture, "initialize", Memory_Profiler_Capture_initialize, 0);
	rb_define_method(Memory_Profiler_Capture, "start", Memory_Profiler_Capture_start, 0);
	rb_define_method(Memory_Profiler_Capture, "stop", Memory_Profiler_Capture_stop, 0);
	rb_define_method(Memory_Profiler_Capture, "track", Memory_Profiler_Capture_track, -1);  // -1 to accept block
	rb_define_method(Memory_Profiler_Capture, "untrack", Memory_Profiler_Capture_untrack, 1);
	rb_define_method(Memory_Profiler_Capture, "tracking?", Memory_Profiler_Capture_tracking_p, 1);
	rb_define_method(Memory_Profiler_Capture, "retained_count_of", Memory_Profiler_Capture_retained_count_of, 1);
	rb_define_method(Memory_Profiler_Capture, "each", Memory_Profiler_Capture_each, 0);
	rb_define_method(Memory_Profiler_Capture, "each_object", Memory_Profiler_Capture_each_object, -1);  // -1 = variable args
	rb_define_method(Memory_Profiler_Capture, "[]", Memory_Profiler_Capture_aref, 1);
	rb_define_method(Memory_Profiler_Capture, "clear", Memory_Profiler_Capture_clear, 0);
	rb_define_method(Memory_Profiler_Capture, "statistics", Memory_Profiler_Capture_statistics, 0);
	rb_define_method(Memory_Profiler_Capture, "new_count", Memory_Profiler_Capture_new_count, 0);
	rb_define_method(Memory_Profiler_Capture, "free_count", Memory_Profiler_Capture_free_count, 0);
	rb_define_method(Memory_Profiler_Capture, "retained_count", Memory_Profiler_Capture_retained_count, 0);
	
	// Initialize Allocations class
	Init_Memory_Profiler_Allocations(Memory_Profiler);
}
