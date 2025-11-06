// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "events.h"
#include "capture.h"

#include <ruby/debug.h>
#include <stdio.h>

enum {
	DEBUG = 0,
};

// Internal structure for the global event queue system.
struct Memory_Profiler_Events {
	// The VALUE wrapper for this struct (needed for write barriers).
	VALUE self;
	
	// Double-buffered event queues (contains events from all Capture instances).
	struct Memory_Profiler_Queue queues[2];
	struct Memory_Profiler_Queue *available, *processing;

	// Postponed job handle for processing the queue.
	// Postponed job handles are an extremely limited resource, so we only register one global event queue.
	rb_postponed_job_handle_t postponed_job_handle;
};

static void Memory_Profiler_Events_process_queue(void *arg);
static void Memory_Profiler_Events_mark(void *ptr);
static void Memory_Profiler_Events_compact(void *ptr);
static void Memory_Profiler_Events_free(void *ptr);
static size_t Memory_Profiler_Events_memsize(const void *ptr);
static const char *Memory_Profiler_Event_Type_name(enum Memory_Profiler_Event_Type type);

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
	
	// Initialize both queues for double buffering:
	Memory_Profiler_Queue_initialize(&events->queues[0], sizeof(struct Memory_Profiler_Event));
	Memory_Profiler_Queue_initialize(&events->queues[1], sizeof(struct Memory_Profiler_Event));

	// Start with queues[0] available for incoming events, queues[1] for processing (initially empty):
	events->available = &events->queues[0];
	events->processing = &events->queues[1];
	
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

// Helper to mark events in a queue.
static void Memory_Profiler_Events_mark_queue(struct Memory_Profiler_Queue *queue, int skip_none) {
	for (size_t i = 0; i < queue->count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(queue, i);
		
		// Skip already-processed events if requested:
		if (skip_none && event->type == MEMORY_PROFILER_EVENT_TYPE_NONE) continue;
		
		rb_gc_mark_movable(event->capture);
		rb_gc_mark_movable(event->klass);
		rb_gc_mark_movable(event->object);
	}
}

// GC mark callback - mark all VALUEs in both event queues.
static void Memory_Profiler_Events_mark(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	
	// Mark all events in the available queue (receiving new events):
	Memory_Profiler_Events_mark_queue(events->available, 0);
	
	// Mark all events in the processing queue (currently being processed):
	Memory_Profiler_Events_mark_queue(events->processing, 1);
}

// Helper to compact events in a queue.
static void Memory_Profiler_Events_compact_queue(struct Memory_Profiler_Queue *queue, int skip_none) {
	for (size_t i = 0; i < queue->count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(queue, i);
		
		// Skip already-processed events if requested:
		if (skip_none && event->type == MEMORY_PROFILER_EVENT_TYPE_NONE) continue;
		
		event->capture = rb_gc_location(event->capture);
		event->klass = rb_gc_location(event->klass);
		event->object = rb_gc_location(event->object);
	}
}

// GC compact callback - update all VALUEs in both event queues.
static void Memory_Profiler_Events_compact(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	
	// Update objects in the available queue:
	Memory_Profiler_Events_compact_queue(events->available, 0);
	
	// Update objects in the processing queue:
	Memory_Profiler_Events_compact_queue(events->processing, 1);
}

// GC free callback.
static void Memory_Profiler_Events_free(void *ptr) {
	struct Memory_Profiler_Events *events = ptr;
	Memory_Profiler_Queue_free(&events->queues[0]);
	Memory_Profiler_Queue_free(&events->queues[1]);
}

// GC memsize callback.
static size_t Memory_Profiler_Events_memsize(const void *ptr) {
	const struct Memory_Profiler_Events *events = ptr;
	return sizeof(struct Memory_Profiler_Events) 
		+ (events->queues[0].capacity * events->queues[0].element_size)
		+ (events->queues[1].capacity * events->queues[1].element_size);
}

const char *Memory_Profiler_Event_Type_name(enum Memory_Profiler_Event_Type type) {
	switch (type) {
		case MEMORY_PROFILER_EVENT_TYPE_NEWOBJ:
			return "NEWOBJ";
		case MEMORY_PROFILER_EVENT_TYPE_FREEOBJ:
			return "FREEOBJ";
		default:
			return "NONE";
	}
}

// Enqueue an event to the available queue (can be called anytime, even during processing).
int Memory_Profiler_Events_enqueue(
	enum Memory_Profiler_Event_Type type,
	VALUE capture,
	VALUE klass,
	VALUE object_id
) {
	struct Memory_Profiler_Events *events = Memory_Profiler_Events_instance();
	
	// Always enqueue to the available queue - it won't be touched during processing:
	struct Memory_Profiler_Event *event = Memory_Profiler_Queue_push(events->available);
	if (event) {
		event->type = type;
		
		// Use write barriers when storing VALUEs (required for RUBY_TYPED_WB_PROTECTED):
		RB_OBJ_WRITE(events->self, &event->capture, capture);
		RB_OBJ_WRITE(events->self, &event->klass, klass);
		RB_OBJ_WRITE(events->self, &event->object, object_id);
		
		if (DEBUG) fprintf(stderr, "Queued %s to available queue, size: %zu\n", 
			Memory_Profiler_Event_Type_name(type), events->available->count);
		
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
	
	// Swap the queues: available becomes processing, and the old processing queue (now empty) becomes available. This allows new events to continue enqueueing to the new available queue while we process.
	struct Memory_Profiler_Queue *queue_to_process = events->available;
	events->available = events->processing;
	events->processing = queue_to_process;
	
	if (DEBUG) fprintf(stderr, "Processing event queue: %zu events\n", events->processing->count);

	// Process all events in order (maintains NEWOBJ before FREEOBJ for same object):
	for (size_t i = 0; i < events->processing->count; i++) {
		struct Memory_Profiler_Event *event = Memory_Profiler_Queue_at(events->processing, i);
		
		// Process event with rb_protect to catch any exceptions:
		int state = 0;
		rb_protect(Memory_Profiler_Events_process_event_protected, (VALUE)event, &state);
		
		if (state) {
			// Exception occurred, warn and suppress:
			rb_warning("Exception in event processing callback (caught and suppressed): %"PRIsVALUE, rb_errinfo());
			rb_set_errinfo(Qnil);
		}
		
		// Clear this event after processing to prevent marking stale data if GC runs:
		event->type = MEMORY_PROFILER_EVENT_TYPE_NONE;
		RB_OBJ_WRITE(events->self, &event->capture, Qnil);
		RB_OBJ_WRITE(events->self, &event->klass, Qnil);
		RB_OBJ_WRITE(events->self, &event->object, Qnil);
	}
	
	// Clear the processing queue (which is now empty logically):
	Memory_Profiler_Queue_clear(events->processing);
}
