// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "capture.h"
#include "allocations.h"

#include "ruby.h"
#include "ruby/debug.h"
#include "ruby/st.h"
#include <stdatomic.h>
#include <stdio.h>

enum {
	DEBUG = 0,
};

static VALUE Memory_Profiler_Capture = Qnil;

// Event symbols
static VALUE sym_newobj;
static VALUE sym_freeobj;

// Main capture state
struct Memory_Profiler_Capture {
	// class => VALUE (wrapped Memory_Profiler_Capture_Allocations).
	st_table *tracked_classes;

	// Is tracking enabled (via start/stop):
	int enabled;
};

// GC mark callback for tracked_classes table
static int Memory_Profiler_Capture_tracked_classes_mark(st_data_t key, st_data_t value, st_data_t arg) {
	// Mark class as un-movable as we don't want it moving in freeobj.
	VALUE klass = (VALUE)key;
	rb_gc_mark(klass);
	
	// Mark the wrapped Allocations VALUE (its own mark function will handle internal refs)
	VALUE allocations = (VALUE)value;
	rb_gc_mark_movable(allocations);
	
	return ST_CONTINUE;
}

// GC mark function
static void Memory_Profiler_Capture_mark(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
	
	if (!capture) {
		return;
	}
	
	if (capture->tracked_classes) {
		st_foreach(capture->tracked_classes, Memory_Profiler_Capture_tracked_classes_mark, 0);
	}
}

// GC free function
static void Memory_Profiler_Capture_free(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
		
	if (capture->tracked_classes) {
		st_free_table(capture->tracked_classes);
	}
	
	xfree(capture);
}

// GC memsize function
static size_t Memory_Profiler_Capture_memsize(const void *ptr) {
	const struct Memory_Profiler_Capture *capture = ptr;
	size_t size = sizeof(struct Memory_Profiler_Capture);
	
	if (capture->tracked_classes) {
		size += capture->tracked_classes->num_entries * (sizeof(st_data_t) + sizeof(struct Memory_Profiler_Capture_Allocations));
	}
	
	return size;
}

// Foreach callback for st_foreach_with_replace (iteration logic)
static int Memory_Profiler_Capture_tracked_classes_foreach(st_data_t key, st_data_t value, st_data_t argp, int error) {
	// Return ST_REPLACE to trigger the replace callback for each entry
	return ST_REPLACE;
}

// Replace callback for st_foreach_with_replace (update logic)
static int Memory_Profiler_Capture_tracked_classes_update(st_data_t *key, st_data_t *value, st_data_t argp, int existing) {
	// Update class key if it moved
	VALUE old_klass = (VALUE)*key;
	VALUE new_klass = rb_gc_location(old_klass);
	if (old_klass != new_klass) {
		*key = (st_data_t)new_klass;
	}
	
	// Update wrapped Allocations VALUE if it moved (its own compact function handles internal refs)
	VALUE old_allocations = (VALUE)*value;
	VALUE new_allocations = rb_gc_location(old_allocations);
	if (old_allocations != new_allocations) {
		*value = (st_data_t)new_allocations;
	}
	
	return ST_CONTINUE;
}

// GC compact function - update VALUEs when GC compaction moves objects
static void Memory_Profiler_Capture_compact(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
	
	// Update tracked_classes keys and callback values in-place
	if (capture->tracked_classes && capture->tracked_classes->num_entries > 0) {
		if (st_foreach_with_replace(capture->tracked_classes, Memory_Profiler_Capture_tracked_classes_foreach, Memory_Profiler_Capture_tracked_classes_update, 0)) {
			rb_raise(rb_eRuntimeError, "tracked_classes modified during GC compaction");
		}
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

// Handler for NEWOBJ event
static void Memory_Profiler_Capture_newobj_handler(VALUE self, struct Memory_Profiler_Capture *capture, VALUE klass, VALUE object) {
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		record->new_count++;
		if (!NIL_P(record->callback)) {
			// Invoke callback - runs during NEWOBJ with GC disabled
			// CRITICAL CALLBACK REQUIREMENTS:
			// - Must be FAST (runs on EVERY allocation)
			// - Must NOT call GC.start (will deadlock)
			// - Must NOT block/sleep (stalls all allocations system-wide)
			// - Should NOT raise exceptions (will propagate to allocating code)
			// - Avoid allocating objects (causes re-entry)
			
			// Call with (klass, :newobj, nil) - callback returns state to store
			VALUE state = rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_newobj, Qnil);
			
			// Store the state if callback returned something
			if (!NIL_P(state)) {
				if (!record->object_states) {
					record->object_states = st_init_numtable();
				}
				st_insert(record->object_states, (st_data_t)object, (st_data_t)state);
				// Notify GC about the state VALUE stored in the table
				RB_OBJ_WRITTEN(self, Qnil, state);
			}
		}
	} else {
		// Create record for this class (first time seeing it)
		struct Memory_Profiler_Capture_Allocations *record = ALLOC(struct Memory_Profiler_Capture_Allocations);
		record->callback = Qnil;
		record->new_count = 1;  // This is the first allocation
		record->free_count = 0;
		record->object_states = NULL;
		
		// Wrap the record in a VALUE
		VALUE allocations = Memory_Profiler_Allocations_wrap(record);
		
		st_insert(capture->tracked_classes, (st_data_t)klass, (st_data_t)allocations);
		// Notify GC about the class VALUE stored as key in the table
		RB_OBJ_WRITTEN(self, Qnil, klass);
		// Notify GC about the allocations VALUE stored as value in the table
		RB_OBJ_WRITTEN(self, Qnil, allocations);
	}
}

// Handler for FREEOBJ event
static void Memory_Profiler_Capture_freeobj_handler(VALUE self, struct Memory_Profiler_Capture *capture, VALUE klass, VALUE object) {
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		record->free_count++;
		if (!NIL_P(record->callback) && record->object_states) {
			// Look up state stored during NEWOBJ
			st_data_t state_data;
			if (st_delete(record->object_states, (st_data_t *)&object, &state_data)) {
				VALUE state = (VALUE)state_data;
				// Call with (klass, :freeobj, state)
				rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_freeobj, state);
			}
		}
	}
}

// Check if object type is trackable (has valid class pointer and is a normal Ruby object)
// Excludes internal types (T_IMEMO, T_NODE, T_ICLASS, etc.) that don't have normal classes
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
static void Memory_Profiler_Capture_event_callback(VALUE data, void *ptr) {
	rb_trace_arg_t *trace_arg = (rb_trace_arg_t *)ptr;
	
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(data, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture->enabled) return;
	
	VALUE object = rb_tracearg_object(trace_arg);

	// We don't want to track internal non-Object allocations:
	if (!Memory_Profiler_Capture_trackable_p(object)) return;

	rb_event_flag_t event_flag = rb_tracearg_event_flag(trace_arg);
	VALUE klass = rb_class_of(object);
	if (!klass) return;
	
	if (DEBUG) {
		const char *klass_name = rb_class2name(klass);
		fprintf(stderr, "Memory_Profiler_Capture_event_callback: %s, Object: %p, Class: %p (%s)\n", event_flag_name(event_flag), (void *)object, (void *)klass, klass_name);
	}
	
	if (event_flag == RUBY_INTERNAL_EVENT_NEWOBJ) {
		// self is the newly allocated object
		Memory_Profiler_Capture_newobj_handler(data, capture, klass, object);
	} else if (event_flag == RUBY_INTERNAL_EVENT_FREEOBJ) {
		// self is the object being freed
		Memory_Profiler_Capture_freeobj_handler(data, capture, klass, object);
	}
}

// Allocate new capture
static VALUE Memory_Profiler_Capture_alloc(VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	VALUE obj = TypedData_Make_Struct(klass, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture) {
		rb_raise(rb_eRuntimeError, "Failed to allocate Memory::Profiler::Capture");
	}
	
	capture->tracked_classes = st_init_numtable();
	
	if (!capture->tracked_classes) {
		rb_raise(rb_eRuntimeError, "Failed to initialize hash table");
	}
	
	capture->enabled = 0;
	
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
	
	if (capture->enabled) return Qfalse;
	
	// Add event hook for NEWOBJ and FREEOBJ with RAW_ARG to get trace_arg
	rb_add_event_hook2(
		(rb_event_hook_func_t)Memory_Profiler_Capture_event_callback,
		RUBY_INTERNAL_EVENT_NEWOBJ | RUBY_INTERNAL_EVENT_FREEOBJ,
		self,
		RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG
	);
	
	capture->enabled = 1;
	
	return Qtrue;
}

// Stop capturing allocations
static VALUE Memory_Profiler_Capture_stop(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture->enabled) return Qfalse;
	
	// Remove event hook using same data (self) we registered with
	rb_remove_event_hook_with_data((rb_event_hook_func_t)Memory_Profiler_Capture_event_callback, self);
	
	capture->enabled = 0;
	
	return Qtrue;
}

// Add a class to track with optional callback
// Usage: track(klass) or track(klass) { |obj, klass| ... }
// Callback can call caller_locations with desired depth
static VALUE Memory_Profiler_Capture_track(int argc, VALUE *argv, VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE klass, callback;
	rb_scan_args(argc, argv, "1&", &klass, &callback);
		
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		RB_OBJ_WRITE(self, &record->callback, callback);
	} else {
		struct Memory_Profiler_Capture_Allocations *record = ALLOC(struct Memory_Profiler_Capture_Allocations);
		record->callback = callback;  // Initial assignment, no write barrier needed
		record->new_count = 0;
		record->free_count = 0;
		record->object_states = NULL;
		
		// Wrap the record in a VALUE
		VALUE allocations = Memory_Profiler_Allocations_wrap(record);
		
		st_insert(capture->tracked_classes, (st_data_t)klass, (st_data_t)allocations);
		// Notify GC about the class VALUE stored as key in the table
		RB_OBJ_WRITTEN(self, Qnil, klass);
		// Notify GC about the allocations VALUE stored as value in the table
		RB_OBJ_WRITTEN(self, Qnil, allocations);
		// Now inform GC about the callback reference
		if (!NIL_P(callback)) {
			RB_OBJ_WRITTEN(self, Qnil, callback);
		}
	}
	
	return self;
}

// Stop tracking a class
static VALUE Memory_Profiler_Capture_untrack(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_delete(capture->tracked_classes, (st_data_t *)&klass, &allocations_data)) {
		// The wrapped Allocations VALUE will be GC'd naturally
		// No manual cleanup needed
	}
	
	return self;
}

// Check if tracking a class
static VALUE Memory_Profiler_Capture_tracking_p(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	return st_lookup(capture->tracked_classes, (st_data_t)klass, NULL) ? Qtrue : Qfalse;
}


// Get count of live objects for a specific class (O(1) lookup!)
static VALUE Memory_Profiler_Capture_count_for(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		// Return net live count (new_count - free_count)
		// Handle case where more objects freed than allocated (allocated before tracking started)
		if (record->free_count > record->new_count) {
			return INT2FIX(0);  // Can't have negative live count
		}
		size_t live_count = record->new_count - record->free_count;
		return SIZET2NUM(live_count);
	}
	
	return INT2FIX(0);
}

// Iterator to reset each class record
static int Memory_Profiler_Capture_tracked_classes_clear(st_data_t key, st_data_t value, st_data_t arg) {
	VALUE allocations = (VALUE)value;
	
	Memory_Profiler_Allocations_clear(allocations);
	
	return ST_CONTINUE;
}

// Clear all allocation tracking (resets all counts to 0)
static VALUE Memory_Profiler_Capture_clear(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	// Reset all counts to 0 (don't free, just reset) - pass self for write barriers
	st_foreach(capture->tracked_classes, Memory_Profiler_Capture_tracked_classes_clear, 0);
	
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
	
	st_foreach(capture->tracked_classes, Memory_Profiler_Capture_each_allocation, 0);
	
	return self;
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
	rb_define_method(Memory_Profiler_Capture, "count_for", Memory_Profiler_Capture_count_for, 1);
	rb_define_method(Memory_Profiler_Capture, "each", Memory_Profiler_Capture_each, 0);
	rb_define_method(Memory_Profiler_Capture, "clear", Memory_Profiler_Capture_clear, 0);
	
	// Initialize Allocations class
	Init_Memory_Profiler_Allocations(Memory_Profiler);
}
