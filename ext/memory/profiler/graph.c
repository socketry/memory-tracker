// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "graph.h"
#include "allocations.h"
#include <ruby/st.h>
#include <stdio.h>

static VALUE Memory_Profiler_Graph = Qnil;

#ifdef HAVE_RB_OBJSPACE_REACHABLE_OBJECTS_FROM
#define MEMORY_PROFILER_GRAPH_REACHABLE_NATIVE
#endif

#ifdef MEMORY_PROFILER_GRAPH_REACHABLE_NATIVE
// Declare native objspace functions:
extern void rb_objspace_reachable_objects_from(VALUE object, void (*func)(VALUE, void *), void *data);
extern int rb_objspace_internal_object_p(VALUE object);
extern int rb_objspace_garbage_object_p(VALUE object);

static void Memory_Profiler_Graph_reachable_objects_from_callback(VALUE object, void *data) {	
	// Skip garbage objects and internal objects:
	if (rb_objspace_garbage_object_p(object)) {
		return;
	}
	
	if (rb_objspace_internal_object_p(object)) {
		return;
	}
	
	rb_yield_values(1, object);
}

static VALUE Memory_Profiler_Graph_reachable_objects_from(VALUE self, VALUE object) {
	rb_objspace_reachable_objects_from(object, Memory_Profiler_Graph_reachable_objects_from_callback, NULL);

	return Qnil;
}
#else
static VALUE rb_mObjectSpace = Qnil;
static VALUE rb_cInternalObjectWrapper = Qnil;
static ID reachable_objects_from_id;

static void Memory_Profiler_Graph_reachable_objects_from(VALUE self, VALUE object) {
	VALUE reachable = rb_funcall(rb_mObjectSpace, reachable_objects_from_id, 1, obj);
	size_t length = RARRAY_LEN(reachable);
	
	for (size_t i = 0; i < length; i++) {
		VALUE child = rb_ary_entry(reachable, i);

		// Skip internal objects:
		if (rb_obj_is_kind_of(child, rb_cInternalObjectWrapper)) {
			continue;
		}
		
		rb_yield_values(1, child);
	}

	return Qnil;
}
#endif

void Init_Memory_Profiler_Graph(VALUE Memory_Profiler)
{
#ifndef MEMORY_PROFILER_GRAPH_REACHABLE_NATIVE
	rb_mObjectSpace = rb_const_get(rb_cObject, rb_intern("ObjectSpace"));
	rb_gc_register_mark_object(rb_mObjectSpace);
	
	// Try to get InternalObjectWrapper class for filtering (may not exist until objspace is loaded)
	rb_cInternalObjectWrapper = rb_const_get_from(rb_mObjectSpace, rb_intern("InternalObjectWrapper"));
	rb_gc_register_mark_object(rb_cInternalObjectWrapper);

	reachable_objects_from_id = rb_intern("reachable_objects_from");
#endif
	
	// Define Graph class
	Memory_Profiler_Graph = rb_define_class_under(Memory_Profiler, "Graph", rb_cObject);
	
	// Define the C methods
	rb_define_method(Memory_Profiler_Graph, "reachable_objects_from", Memory_Profiler_Graph_reachable_objects_from, 1);
}
