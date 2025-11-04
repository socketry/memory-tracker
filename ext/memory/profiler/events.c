// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "ruby.h"
#include "ruby/debug.h"

#include "events.h"
#include "capture.h"
#include <stdio.h>

enum {
	DEBUG = 0,
};

// Internal structure for the global event queue system.
struct Memory_Profiler_Events {
	// The VALUE wrapper for this struct (needed for write barriers).
	VALUE self;
	
	// Global event queue (contains events from all Capture instances).
	struct Memory_Profiler_Queue queue;
	
	// Postponed job handle for processing the queue.
	// Postponed job handles are an extremely limited resource, so we only register one global event queue.
	rb_postponed_job_handle_t postponed_job_handle;
};

static void Memory_Profiler_Events_process_queue(void *arg);
static void Memory_Profiler_Events_mark(void *ptr);
static void Memory_Profiler_Events_compact(void *ptr);
static void Memory_Profiler_Events_free(void *ptr);
static size_t Memory_Profiler_Events_memsize(const void *ptr);

// TypedData definition for Events.
static const rb_data_type_t Memory_Profiler_Events_type = {
	"Memory::Profiler::Events",
	{
		.dmark = Memory_Profiler_Events_mark,
		.dcompact = Memory_Profiler_Events_compact,
		.dfree = Memory_Profiler_Events_free,
		.dsize = Memory_Profiler_Events_memsize,
	},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

// Create and initialize the global event queue system.
static VALUE Memory_Profiler_Events_new(void) {
	struct Memory_Profiler_Events *events;
	VALUE self = TypedData_Make_Struct(rb_cObject, struct Memory_Profiler_Events, &Memory_Profiler_Events_type, events);
	
	// Store the VALUE wrapper for write barriers:
	events->self = self;
	
	// Initialize the global event queue:
	Memory_Profiler_Queue_initialize(&events->queue, sizeof(struct Memory_Profiler_Event));
	
	// Pre-register the single postponed job for processing the queue:
	events->postponed_job_handle = rb_postponed_job_preregister(0,
		// Callback function to process the queue:
		Memory_Profiler_Events_process_queue,
		// Pass the events struct as argument:
		(void *)events
	);
	
	if (events->postponed_job_handle == POSTPONED_JOB_HANDLE_INVALID) {
		rb_raise(rb_eRuntimeError, "Failed to register postponed job!");
	}
	
	return self;
}

// Get the global events instance (internal helper).
struct Memory_Profiler_Events* Memory_Profiler_Events_instance(void) {
	static VALUE instance = Qnil;
	static struct Memory_Profiler_Events *events = NULL;

	if (instance == Qnil) {
		instance = Memory_Profiler_Events_new();
		
		// Pin the global events object so it's never GC'd:
		rb_gc_register_mark_object(instance);
		
		if (DEBUG) fprintf(stderr, "Global event queue system initialized and pinned\n");
		
		TypedData_Get_Struct(instance, struct Memory_Profiler_Events, &Memory_Profiler_Events_type, events);
	}
	
	return events;
}

// GC mark callback - mark all VALUEs in the event queue.
static void Memory_Profiler_Events_mark(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	
	// Mark all events in the global queue:
	for (size_t i = 0; i < events->queue.count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(&events->queue, i);
		
		// Mark the Capture instance this event belongs to:
		rb_gc_mark_movable(event->capture);
		rb_gc_mark_movable(event->klass);
		rb_gc_mark_movable(event->allocations);
		
		// For NEWOBJ, mark the object (it's alive).
		// For FREEOBJ, DON'T mark (it's being freed - just used as key for lookup).
		if (event->type == MEMORY_PROFILER_EVENT_TYPE_NEWOBJ) {
			rb_gc_mark_movable(event->object);
		}
	}
}

// GC compact callback - update all VALUEs in the event queue.
static void Memory_Profiler_Events_compact(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	
	// Update objects in the global event queue:
	for (size_t i = 0; i < events->queue.count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(&events->queue, i);
		
		// Update all VALUEs if they moved during compaction:
		event->capture = rb_gc_location(event->capture);
		event->klass = rb_gc_location(event->klass);
		event->allocations = rb_gc_location(event->allocations);
		
		// For NEWOBJ, update the object pointer.
		// For FREEOBJ, DON'T update (it's being freed, pointer is stale).
		if (event->type == MEMORY_PROFILER_EVENT_TYPE_NEWOBJ) {
			event->object = rb_gc_location(event->object);
		}
	}
}

// GC free callback.
static void Memory_Profiler_Events_free(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	Memory_Profiler_Queue_free(&events->queue);
}

// GC memsize callback.
static size_t Memory_Profiler_Events_memsize(const void *ptr) {
	const struct Memory_Profiler_Events *events = ptr;
	return sizeof(struct Memory_Profiler_Events) + (events->queue.capacity * events->queue.element_size);
}

// Enqueue an event to the global queue.
int Memory_Profiler_Events_enqueue(
	enum Memory_Profiler_Event_Type type,
	VALUE capture,
	VALUE klass,
	VALUE allocations,
	VALUE object
) {
	struct Memory_Profiler_Events *events = Memory_Profiler_Events_instance();
	
	struct Memory_Profiler_Event *event = Memory_Profiler_Queue_push(&events->queue);
	if (event) {
		event->type = type;
		
		// Use write barriers when storing VALUEs (required for RUBY_TYPED_WB_PROTECTED):
		RB_OBJ_WRITE(events->self, &event->capture, capture);
		RB_OBJ_WRITE(events->self, &event->klass, klass);
		RB_OBJ_WRITE(events->self, &event->allocations, allocations);
		RB_OBJ_WRITE(events->self, &event->object, object);
		
		const char *type_name = (type == MEMORY_PROFILER_EVENT_TYPE_NEWOBJ) ? "NEWOBJ" : "FREEOBJ";
		if (DEBUG) fprintf(stderr, "Queued %s to global queue, size: %zu\n", type_name, events->queue.count);
		
		rb_postponed_job_trigger(events->postponed_job_handle);
		// Success:
		return 1;
	}

	// Queue full:
	return 0;
}

// Process all queued events immediately (flush the queue).
// Public API function - called from Capture stop() to ensure all events are processed.
void Memory_Profiler_Events_process_all(void) {
	struct Memory_Profiler_Events *events = Memory_Profiler_Events_instance();
	Memory_Profiler_Events_process_queue((void *)events);
}

// Wrapper for rb_protect - processes a single event.
// rb_protect requires signature: VALUE func(VALUE arg).
static VALUE Memory_Profiler_Events_process_event_protected(VALUE arg) {
	struct Memory_Profiler_Event *event = (struct Memory_Profiler_Event *)arg;
	Memory_Profiler_Capture_process_event(event);
	return Qnil;
}

// Postponed job callback - processes global event queue.
// This runs when it's safe to call Ruby code (not during allocation or GC).
// Processes events from ALL Capture instances.
static void Memory_Profiler_Events_process_queue(void *arg) {
	struct Memory_Profiler_Events *events = (struct Memory_Profiler_Events *)arg;
	
	if (DEBUG) fprintf(stderr, "Processing global event queue: %zu events\n", events->queue.count);
	
	// Process all events in order (maintains NEWOBJ before FREEOBJ for same object):
	for (size_t i = 0; i < events->queue.count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(&events->queue, i);
		
		// Process event with rb_protect to catch any exceptions:
		int state = 0;
		rb_protect(Memory_Profiler_Events_process_event_protected, (VALUE)event, &state);
		
		if (state) {
			// Exception occurred, warn and suppress:
			rb_warning("Exception in event processing callback (caught and suppressed): %"PRIsVALUE, rb_errinfo());
			rb_set_errinfo(Qnil);
		}
	}
	
	// Always clear the global queue, even if exceptions occurred:
	Memory_Profiler_Queue_clear(&events->queue);
}
