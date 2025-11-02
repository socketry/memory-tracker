// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "allocations.h"

#include "ruby.h"
#include "ruby/debug.h"
#include "ruby/st.h"
#include <stdio.h>

static VALUE Memory_Profiler_Allocations = Qnil;

// Helper to mark object_states table values
static int Memory_Profiler_Allocations_object_states_mark(st_data_t key, st_data_t value, st_data_t arg) {
	VALUE object = (VALUE)key;
	rb_gc_mark_movable(object);

	VALUE state = (VALUE)value;
	if (!NIL_P(state)) {
		rb_gc_mark_movable(state);
	}
	return ST_CONTINUE;
}

// Foreach callback for st_foreach_with_replace (iteration logic)
static int Memory_Profiler_Allocations_object_states_foreach(st_data_t key, st_data_t value, st_data_t argp, int error) {
	// Return ST_REPLACE to trigger the replace callback for each entry
	return ST_REPLACE;
}

// Replace callback for st_foreach_with_replace to update object_states keys and values during compaction
static int Memory_Profiler_Allocations_object_states_compact(st_data_t *key, st_data_t *value, st_data_t data, int existing) {
	VALUE old_object = (VALUE)*key;
	VALUE old_state = (VALUE)*value;
	
	VALUE new_object = rb_gc_location(old_object);
	VALUE new_state = rb_gc_location(old_state);
	
	// Update key if it moved
	if (old_object != new_object) {
		*key = (st_data_t)new_object;
	}
	
	// Update value if it moved
	if (old_state != new_state) {
		*value = (st_data_t)new_state;
	}
	
	return ST_CONTINUE;
}

// GC mark function for Allocations
static void Memory_Profiler_Allocations_mark(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	if (!record) {
		return;
	}
	
	if (!NIL_P(record->callback)) {
		rb_gc_mark_movable(record->callback);
	}
	
	// Mark object_states table if it exists
	if (record->object_states) {
		st_foreach(record->object_states, Memory_Profiler_Allocations_object_states_mark, 0);
	}
}

// GC free function for Allocations
static void Memory_Profiler_Allocations_free(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	if (record->object_states) {
		st_free_table(record->object_states);
	}
	
	xfree(record);
}

// GC compact function for Allocations
static void Memory_Profiler_Allocations_compact(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	// Update callback if it moved
	if (!NIL_P(record->callback)) {
		record->callback = rb_gc_location(record->callback);
	}
	
	// Update object_states table if it exists
	if (record->object_states && record->object_states->num_entries > 0) {
		if (st_foreach_with_replace(record->object_states, Memory_Profiler_Allocations_object_states_foreach, Memory_Profiler_Allocations_object_states_compact, 0)) {
			rb_raise(rb_eRuntimeError, "object_states modified during GC compaction");
		}
	}
}

static const rb_data_type_t Memory_Profiler_Allocations_type = {
	"Memory::Profiler::Allocations",
	{
		.dmark = Memory_Profiler_Allocations_mark,
		.dcompact = Memory_Profiler_Allocations_compact,
		.dfree = Memory_Profiler_Allocations_free,
	},
	0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

// Wrap an allocations record
VALUE Memory_Profiler_Allocations_wrap(struct Memory_Profiler_Capture_Allocations *record) {
	return TypedData_Wrap_Struct(Memory_Profiler_Allocations, &Memory_Profiler_Allocations_type, record);
}

// Get allocations record from wrapper
struct Memory_Profiler_Capture_Allocations* Memory_Profiler_Allocations_get(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture_Allocations, &Memory_Profiler_Allocations_type, record);
	return record;
}

// Allocations#new_count
static VALUE Memory_Profiler_Allocations_new_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	return SIZET2NUM(record->new_count);
}

// Allocations#free_count
static VALUE Memory_Profiler_Allocations_free_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	return SIZET2NUM(record->free_count);
}

// Allocations#retained_count
static VALUE Memory_Profiler_Allocations_retained_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	// Handle underflow when free_count > new_count
	size_t retained = record->free_count > record->new_count ? 0 : record->new_count - record->free_count;
	return SIZET2NUM(retained);
}

// Allocations#track { |klass| ... }
static VALUE Memory_Profiler_Allocations_track(int argc, VALUE *argv, VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	
	VALUE callback;
	rb_scan_args(argc, argv, "&", &callback);
	
	// Use write barrier - self (Allocations wrapper) keeps Capture alive, which keeps callback alive
	RB_OBJ_WRITE(self, &record->callback, callback);
	
	return self;
}

// Clear/reset allocation counts and state for a record
void Memory_Profiler_Allocations_clear(VALUE allocations) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
	record->new_count = 0;   // Reset allocation count
	record->free_count = 0;  // Reset free count
	RB_OBJ_WRITE(allocations, &record->callback, Qnil); // Clear callback with write barrier
	
	// Clear object states
	if (record->object_states) {
		st_free_table(record->object_states);
		record->object_states = NULL;
	}
}

void Init_Memory_Profiler_Allocations(VALUE Memory_Profiler)
{
	// Allocations class - wraps allocation data for a specific class
	Memory_Profiler_Allocations = rb_define_class_under(Memory_Profiler, "Allocations", rb_cObject);
	
	// Allocations objects are only created internally via wrap, never from Ruby:
	rb_undef_alloc_func(Memory_Profiler_Allocations);
	
	rb_define_method(Memory_Profiler_Allocations, "new_count", Memory_Profiler_Allocations_new_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "free_count", Memory_Profiler_Allocations_free_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "retained_count", Memory_Profiler_Allocations_retained_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "track", Memory_Profiler_Allocations_track, -1);  // -1 to accept block
}
