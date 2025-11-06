// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "allocations.h"
#include "events.h"
#include <ruby/debug.h>
#include <stdio.h>

static VALUE Memory_Profiler_Allocations = Qnil;

static void Memory_Profiler_Allocations_mark(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	rb_gc_mark_movable(record->callback);
}

static void Memory_Profiler_Allocations_free(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	xfree(record);
}

static void Memory_Profiler_Allocations_compact(void *ptr) {
	struct Memory_Profiler_Capture_Allocations *record = ptr;
	
	record->callback = rb_gc_location(record->callback);
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

VALUE Memory_Profiler_Allocations_wrap(struct Memory_Profiler_Capture_Allocations *record) {
	return TypedData_Wrap_Struct(Memory_Profiler_Allocations, &Memory_Profiler_Allocations_type, record);
}

struct Memory_Profiler_Capture_Allocations* Memory_Profiler_Allocations_get(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record;
	TypedData_Get_Struct(self, struct Memory_Profiler_Capture_Allocations, &Memory_Profiler_Allocations_type, record);
	return record;
}

static VALUE Memory_Profiler_Allocations_new_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	return SIZET2NUM(record->new_count);
}

static VALUE Memory_Profiler_Allocations_free_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	return SIZET2NUM(record->free_count);
}

// Allocations#retained_count
static VALUE Memory_Profiler_Allocations_retained_count(VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	
	// Handle underflow when free_count > new_count:
	size_t retained = record->free_count > record->new_count ? 0 : record->new_count - record->free_count;

	return SIZET2NUM(retained);
}

static VALUE Memory_Profiler_Allocations_track(int argc, VALUE *argv, VALUE self) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(self);
	
	VALUE callback;
	rb_scan_args(argc, argv, "&", &callback);
	
	RB_OBJ_WRITE(self, &record->callback, callback);
	
	return self;
}

void Memory_Profiler_Allocations_clear(VALUE allocations) {
	struct Memory_Profiler_Capture_Allocations *record = Memory_Profiler_Allocations_get(allocations);
	record->new_count = 0;
	record->free_count = 0;
	RB_OBJ_WRITE(allocations, &record->callback, Qnil);
}

static VALUE Memory_Profiler_Allocations_allocate(VALUE klass) {
	struct Memory_Profiler_Capture_Allocations *record = ALLOC(struct Memory_Profiler_Capture_Allocations);
	record->callback = Qnil;
	record->new_count = 0;
	record->free_count = 0;
	
	return Memory_Profiler_Allocations_wrap(record);
}

void Init_Memory_Profiler_Allocations(VALUE Memory_Profiler)
{
	// Allocations class - wraps allocation data for a specific class
	Memory_Profiler_Allocations = rb_define_class_under(Memory_Profiler, "Allocations", rb_cObject);
	
	// Allow allocation for testing
	rb_define_alloc_func(Memory_Profiler_Allocations, Memory_Profiler_Allocations_allocate);
	
	rb_define_method(Memory_Profiler_Allocations, "new_count", Memory_Profiler_Allocations_new_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "free_count", Memory_Profiler_Allocations_free_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "retained_count", Memory_Profiler_Allocations_retained_count, 0);
	rb_define_method(Memory_Profiler_Allocations, "track", Memory_Profiler_Allocations_track, -1);
}
