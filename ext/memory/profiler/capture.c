// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "capture.h"
#include "allocations.h"
#include "queue.h"

#include "ruby.h"
#include "ruby/debug.h"
#include "ruby/st.h"
#include <stdatomic.h>
#include <stdio.h>

enum {
	DEBUG = 0,
	DEBUG_EVENT_QUEUES = 0,
	DEBUG_STATE = 0,
};

static VALUE Memory_Profiler_Capture = Qnil;

// Event symbols
static VALUE sym_newobj;
static VALUE sym_freeobj;

// Queue item - new object data to be processed via postponed job
struct Memory_Profiler_Newobj_Queue_Item {
	// The class of the new object:
	VALUE klass;

	// The Allocations wrapper:
	VALUE allocations;

	// The newly allocated object:
	VALUE object;
};

// Queue item - freed object data to be processed via postponed job
struct Memory_Profiler_Freeobj_Queue_Item {
	// The class of the freed object:
	VALUE klass;

	// The Allocations wrapper:
	VALUE allocations;

	// The state returned from callback on newobj:
	VALUE state;
};

// Main capture state
struct Memory_Profiler_Capture {
	// class => VALUE (wrapped Memory_Profiler_Capture_Allocations).
	st_table *tracked_classes;

	// Master switch - is tracking active? (set by start/stop)
	int running;
	
	// Internal - should we queue callbacks? (temporarily disabled during queue processing)
	int enabled;
	
	// Queue for new objects (processed via postponed job):
	struct Memory_Profiler_Queue newobj_queue;
	
	// Queue for freed objects (processed via postponed job):
	struct Memory_Profiler_Queue freeobj_queue;
	
	// Handle for the postponed job (processes both queues)
	rb_postponed_job_handle_t postponed_job_handle;
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
	
	// Mark new objects in the queue:
	for (size_t i = 0; i < capture->newobj_queue.count; i++) {
		struct Memory_Profiler_Newobj_Queue_Item *newobj = Memory_Profiler_Queue_at(&capture->newobj_queue, i);
		rb_gc_mark_movable(newobj->klass);
		rb_gc_mark_movable(newobj->allocations);
		rb_gc_mark_movable(newobj->object);
	}
	
	// Mark freed objects in the queue:
	for (size_t i = 0; i < capture->freeobj_queue.count; i++) {
		struct Memory_Profiler_Freeobj_Queue_Item *freed = Memory_Profiler_Queue_at(&capture->freeobj_queue, i);
		rb_gc_mark_movable(freed->klass);
		rb_gc_mark_movable(freed->allocations);

		if (freed->state) {
			rb_gc_mark_movable(freed->state);
		}
	}
}

// GC free function
static void Memory_Profiler_Capture_free(void *ptr) {
	struct Memory_Profiler_Capture *capture = ptr;
		
	if (capture->tracked_classes) {
		st_free_table(capture->tracked_classes);
	}
	
	// Free both queues (elements are stored directly, just free the queues)
	Memory_Profiler_Queue_free(&capture->newobj_queue);
	Memory_Profiler_Queue_free(&capture->freeobj_queue);
	
	xfree(capture);
}

// GC memsize function
static size_t Memory_Profiler_Capture_memsize(const void *ptr) {
	const struct Memory_Profiler_Capture *capture = ptr;
	size_t size = sizeof(struct Memory_Profiler_Capture);
	
	if (capture->tracked_classes) {
		size += capture->tracked_classes->num_entries * (sizeof(st_data_t) + sizeof(struct Memory_Profiler_Capture_Allocations));
	}
	
	// Add size of both queues (elements stored directly)
	size += capture->newobj_queue.capacity * capture->newobj_queue.element_size;
	size += capture->freeobj_queue.capacity * capture->freeobj_queue.element_size;
	
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
	
	// Update new objects in the queue
	for (size_t i = 0; i < capture->newobj_queue.count; i++) {
		struct Memory_Profiler_Newobj_Queue_Item *newobj = Memory_Profiler_Queue_at(&capture->newobj_queue, i);
		
		// Update all VALUEs if they moved during compaction
		newobj->klass = rb_gc_location(newobj->klass);
		newobj->allocations = rb_gc_location(newobj->allocations);
		newobj->object = rb_gc_location(newobj->object);
	}
	
	// Update freed objects in the queue
	for (size_t i = 0; i < capture->freeobj_queue.count; i++) {
		struct Memory_Profiler_Freeobj_Queue_Item *freed = Memory_Profiler_Queue_at(&capture->freeobj_queue, i);
		
		// Update all VALUEs if they moved during compaction
		freed->klass = rb_gc_location(freed->klass);
		freed->allocations = rb_gc_location(freed->allocations);
		freed->state = rb_gc_location(freed->state);
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

// Postponed job callback - processes queued new and freed objects
// This runs when it's safe to call Ruby code (not during allocation or GC)
// IMPORTANT: Process newobj queue first, then freeobj queue to maintain order
static void Memory_Profiler_Capture_process_queues(void *arg) {
	VALUE self = (VALUE)arg;
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Processing queues: %zu newobj, %zu freeobj\n", 
		capture->newobj_queue.count, capture->freeobj_queue.count);
	
	// Disable tracking during queue processing to prevent infinite loop
	// (rb_funcall can allocate, which would trigger more NEWOBJ events)
	int was_enabled = capture->enabled;
	capture->enabled = 0;
	
	// First, process all new objects in the queue
	for (size_t i = 0; i < capture->newobj_queue.count; i++) {
		struct Memory_Profiler_Newobj_Queue_Item *newobj = Memory_Profiler_Queue_at(&capture->newobj_queue, i);
		VALUE klass = newobj->klass;
		VALUE allocations = newobj->allocations;
		VALUE object = newobj->object;
		
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		
		// Call the Ruby callback with (klass, :newobj, nil) - callback returns state to store
		if (!NIL_P(record->callback)) {
			VALUE state = rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_newobj, Qnil);
			
			// Store the state if callback returned something
			if (!NIL_P(state)) {
				if (!record->object_states) {
					record->object_states = st_init_numtable();
				}
				
				if (DEBUG_STATE) fprintf(stderr, "Memory_Profiler_Capture_process_queues: Storing state for object: %p (%s)\n", 
					(void *)object, rb_class2name(klass));

				st_insert(record->object_states, (st_data_t)object, (st_data_t)state);
			}
		}
	}
	
	// Then, process all freed objects in the queue
	for (size_t i = 0; i < capture->freeobj_queue.count; i++) {
		struct Memory_Profiler_Freeobj_Queue_Item *freed = Memory_Profiler_Queue_at(&capture->freeobj_queue, i);
		VALUE klass = freed->klass;
		VALUE allocations = freed->allocations;
		VALUE state = freed->state;
		
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		
		// Call the Ruby callback with (klass, :freeobj, state)
		if (!NIL_P(record->callback)) {
			rb_funcall(record->callback, rb_intern("call"), 3, klass, sym_freeobj, state);
		}
	}
	
	// Clear both queues (elements are reused on next cycle)
	Memory_Profiler_Queue_clear(&capture->newobj_queue);
	Memory_Profiler_Queue_clear(&capture->freeobj_queue);
	
	// Restore tracking state
	capture->enabled = was_enabled;
}

// Handler for NEWOBJ event
// SAFE: No longer calls Ruby code directly - queues for deferred processing
static void Memory_Profiler_Capture_newobj_handler(VALUE self, struct Memory_Profiler_Capture *capture, VALUE klass, VALUE object) {
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		
		// Always track counts (even during queue processing)
		record->new_count++;
		
		// Only queue for callback if tracking is enabled (prevents infinite recursion)
		if (capture->enabled && !NIL_P(record->callback)) {
			// Push a new item onto the queue (returns pointer to write to)
			// NOTE: realloc is safe during allocation (doesn't trigger Ruby allocation)
			struct Memory_Profiler_Newobj_Queue_Item *newobj = Memory_Profiler_Queue_push(&capture->newobj_queue);
			if (newobj) {
				if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Queued newobj, queue size now: %zu/%zu\n", 
					capture->newobj_queue.count, capture->newobj_queue.capacity);
				
				// Write VALUEs with write barriers (combines write + GC notification)
				RB_OBJ_WRITE(self, &newobj->klass, klass);
				RB_OBJ_WRITE(self, &newobj->allocations, allocations);
				RB_OBJ_WRITE(self, &newobj->object, object);
				
				// Trigger postponed job to process the queue
				if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Triggering postponed job to process queues\n");
				rb_postponed_job_trigger(capture->postponed_job_handle);
			} else {
				if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Failed to queue newobj, out of memory\n");
			}
			// If push failed (out of memory), silently drop this newobj event
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
// CRITICAL: This runs during GC when no Ruby code can be executed!
// We MUST NOT call rb_funcall or any Ruby code here - just queue the work.
static void Memory_Profiler_Capture_freeobj_handler(VALUE self, struct Memory_Profiler_Capture *capture, VALUE klass, VALUE object) {
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		VALUE allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		
		// Always track counts (even during queue processing)
		record->free_count++;
		
		// Only queue for callback if tracking is enabled and we have state
		// Note: If NEWOBJ didn't queue (enabled=0), there's no state, so this naturally skips
		if (capture->enabled && !NIL_P(record->callback) && record->object_states) {
			if (DEBUG_STATE) fprintf(stderr, "Memory_Profiler_Capture_freeobj_handler: Looking up state for object: %p\n", (void *)object);

			// Look up state stored during NEWOBJ
			st_data_t state_data;
			if (st_delete(record->object_states, (st_data_t *)&object, &state_data)) {
				if (DEBUG_STATE) fprintf(stderr, "Found state for object: %p\n", (void *)object);
				VALUE state = (VALUE)state_data;
				
				// Push a new item onto the queue (returns pointer to write to)
				// NOTE: realloc is safe during GC (doesn't trigger Ruby allocation)
				struct Memory_Profiler_Freeobj_Queue_Item *freeobj = Memory_Profiler_Queue_push(&capture->freeobj_queue);
				if (freeobj) {
					if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Queued freed object, queue size now: %zu/%zu\n", 
						capture->freeobj_queue.count, capture->freeobj_queue.capacity);
					
					// Write VALUEs with write barriers (combines write + GC notification)
					// Note: We're during GC/FREEOBJ, but write barriers should be safe
					RB_OBJ_WRITE(self, &freeobj->klass, klass);
					RB_OBJ_WRITE(self, &freeobj->allocations, allocations);
					RB_OBJ_WRITE(self, &freeobj->state, state);
					
					// Trigger postponed job to process both queues after GC
					if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Triggering postponed job to process queues after GC\n");
					rb_postponed_job_trigger(capture->postponed_job_handle);
				} else {
					if (DEBUG_EVENT_QUEUES) fprintf(stderr, "Failed to queue freed object, out of memory\n");
				}
				// If push failed (out of memory), silently drop this freeobj event
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
	
	VALUE object = rb_tracearg_object(trace_arg);

	// We don't want to track internal non-Object allocations:
	if (!Memory_Profiler_Capture_trackable_p(object)) return;

	rb_event_flag_t event_flag = rb_tracearg_event_flag(trace_arg);
	VALUE klass = rb_class_of(object);
	if (!klass) return;
	
	if (DEBUG) {
		// In events other than NEWOBJ, we are unable to allocate objects (due to GC), so we simply say "ignored":
		const char *klass_name = "ignored";
		if (event_flag == RUBY_INTERNAL_EVENT_NEWOBJ) {
			klass_name = rb_class2name(klass);
		}
		
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
	
	// Initialize state flags - not running, callbacks disabled
	capture->running = 0;
	capture->enabled = 0;
	
	// Initialize both queues
	Memory_Profiler_Queue_initialize(&capture->newobj_queue, sizeof(struct Memory_Profiler_Newobj_Queue_Item));
	Memory_Profiler_Queue_initialize(&capture->freeobj_queue, sizeof(struct Memory_Profiler_Freeobj_Queue_Item));
	
	// Pre-register the postponed job for processing both queues
	// The job will be triggered whenever we queue newobj or freeobj events
	capture->postponed_job_handle = rb_postponed_job_preregister(
		0, // flags
		Memory_Profiler_Capture_process_queues,
		(void *)obj
	);
	
	if (capture->postponed_job_handle == POSTPONED_JOB_HANDLE_INVALID) {
		rb_raise(rb_eRuntimeError, "Failed to register postponed job!");
	}
	
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
	
	// Add event hook for NEWOBJ and FREEOBJ with RAW_ARG to get trace_arg
	rb_add_event_hook2(
		(rb_event_hook_func_t)Memory_Profiler_Capture_event_callback,
		RUBY_INTERNAL_EVENT_NEWOBJ | RUBY_INTERNAL_EVENT_FREEOBJ,
		self,
		RUBY_EVENT_HOOK_FLAG_SAFE | RUBY_EVENT_HOOK_FLAG_RAW_ARG
	);
	
	// Set both flags - we're now running and callbacks are enabled
	capture->running = 1;
	capture->enabled = 1;
	
	return Qtrue;
}

// Stop capturing allocations
static VALUE Memory_Profiler_Capture_stop(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	if (!capture->running) return Qfalse;
	
	// Remove event hook using same data (self) we registered with. No more events will be queued after this point:
	rb_remove_event_hook_with_data((rb_event_hook_func_t)Memory_Profiler_Capture_event_callback, self);
	
	// Flush any pending queued events before stopping. This ensures all callbacks are invoked and object_states is properly maintained.
	Memory_Profiler_Capture_process_queues((void *)self);
	
	// Clear both flags - we're no longer running and callbacks are disabled
	capture->running = 0;
	capture->enabled = 0;
	
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
	
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		allocations = (VALUE)allocations_data;
		struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
		RB_OBJ_WRITE(self, &record->callback, callback);
	} else {
		struct Memory_Profiler_Capture_Allocations *record = ALLOC(struct Memory_Profiler_Capture_Allocations);
		record->callback = callback;  // Initial assignment, no write barrier needed
		record->new_count = 0;
		record->free_count = 0;
		record->object_states = NULL;
		
		// Wrap the record in a VALUE
		allocations = Memory_Profiler_Allocations_wrap(record);
		
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
	
	return allocations;
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

// Get allocations for a specific class
static VALUE Memory_Profiler_Capture_aref(VALUE self, VALUE klass) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	st_data_t allocations_data;
	if (st_lookup(capture->tracked_classes, (st_data_t)klass, &allocations_data)) {
		return (VALUE)allocations_data;
	}
	
	return Qnil;
}

// Struct to accumulate statistics during iteration
struct Memory_Profiler_Allocations_Statistics {
	size_t total_tracked_objects;
	VALUE per_class_counts;
};

// Iterator callback to count object_states per class
static int Memory_Profiler_Capture_count_object_states(st_data_t key, st_data_t value, st_data_t argument) {
	struct Memory_Profiler_Allocations_Statistics *statistics = (struct Memory_Profiler_Allocations_Statistics *)argument;
	VALUE klass = (VALUE)key;
	VALUE allocations = (VALUE)value;
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
	
	size_t object_states_count = record->object_states ? record->object_states->num_entries : 0;
	statistics->total_tracked_objects += object_states_count;
	
	rb_hash_aset(statistics->per_class_counts, klass, SIZET2NUM(object_states_count));
	return ST_CONTINUE;
}

// Get internal statistics for debugging
// Returns hash with internal state sizes
static VALUE Memory_Profiler_Capture_statistics(VALUE self) {
	struct Memory_Profiler_Capture *capture;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture, &Memory_Profiler_Capture_type, capture);
	
	VALUE statistics = rb_hash_new();
	
	// Tracked classes count
	rb_hash_aset(statistics, ID2SYM(rb_intern("tracked_classes_count")), SIZET2NUM(capture->tracked_classes->num_entries));
	
	// Queue sizes
	rb_hash_aset(statistics, ID2SYM(rb_intern("newobj_queue_size")), SIZET2NUM(capture->newobj_queue.count));
	rb_hash_aset(statistics, ID2SYM(rb_intern("newobj_queue_capacity")), SIZET2NUM(capture->newobj_queue.capacity));
	rb_hash_aset(statistics, ID2SYM(rb_intern("freeobj_queue_size")), SIZET2NUM(capture->freeobj_queue.count));
	rb_hash_aset(statistics, ID2SYM(rb_intern("freeobj_queue_capacity")), SIZET2NUM(capture->freeobj_queue.capacity));
	
	// Count object_states entries for each tracked class
	struct Memory_Profiler_Allocations_Statistics allocations_statistics = {
		.total_tracked_objects = 0,
		.per_class_counts = rb_hash_new()
	};
	
	st_foreach(capture->tracked_classes, Memory_Profiler_Capture_count_object_states, (st_data_t)&allocations_statistics);
	
	rb_hash_aset(statistics, ID2SYM(rb_intern("total_tracked_objects")), SIZET2NUM(allocations_statistics.total_tracked_objects));
	rb_hash_aset(statistics, ID2SYM(rb_intern("tracked_objects_per_class")), allocations_statistics.per_class_counts);
	
	return statistics;
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
	rb_define_method(Memory_Profiler_Capture, "[]", Memory_Profiler_Capture_aref, 1);
	rb_define_method(Memory_Profiler_Capture, "clear", Memory_Profiler_Capture_clear, 0);
	rb_define_method(Memory_Profiler_Capture, "statistics", Memory_Profiler_Capture_statistics, 0);
	
	// Initialize Allocations class
	Init_Memory_Profiler_Allocations(Memory_Profiler);
}
