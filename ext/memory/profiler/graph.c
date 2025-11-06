// Released under the MIT License.
// Copyright, 2025, by Samuel Williams.

#include "graph.h"
#include "allocations.h"

#include "ruby.h"
#include "ruby/st.h"
#include <stdio.h>

static VALUE Memory_Profiler_Graph = Qnil;

#ifdef HAVE_RB_OBJSPACE_REACHABLE_OBJECTS_FROM
#define MEMORY_PROFILER_GRAPH_REACHABLE_NATIVE
#endif

#ifdef MEMORY_PROFILER_GRAPH_REACHABLE_NATIVE
// Declare native objspace functions:
extern void rb_objspace_reachable_objects_from(VALUE obj, void (*func)(VALUE, void *), void *data);
extern int rb_objspace_internal_object_p(VALUE obj);
extern int rb_objspace_garbage_object_p(VALUE obj);

struct Memory_Profiler_Graph_Reachable_Context {
	void (*func)(VALUE, void *);
	void *data;
};

static void Memory_Profiler_Graph_reachable_objects_from_callback(VALUE object, void *data) {
	struct Memory_Profiler_Graph_Reachable_Context *context = (struct Memory_Profiler_Graph_Reachable_Context *)data;
	
	// Skip garbage objects and internal objects:
	if (rb_objspace_garbage_object_p(object)) {
		return;
	}
	
	if (rb_objspace_internal_object_p(object)) {
		return;
	}
	
	// Call the user's callback:
	context->func(object, context->data);
}

static void Memory_Profiler_Graph_reachable_objects_from(VALUE obj, void (*func)(VALUE, void *), void *data) {
	struct Memory_Profiler_Graph_Reachable_Context context = {
		.func = func,
		.data = data
	};
	rb_objspace_reachable_objects_from(obj, Memory_Profiler_Graph_reachable_objects_from_callback, &context);
}
#else
static VALUE rb_mObjectSpace = Qnil;
static VALUE rb_cInternalObjectWrapper = Qnil;
static ID reachable_objects_from_id;

static void Memory_Profiler_Graph_reachable_objects_from(VALUE obj, void (*func)(VALUE, void *), void *data) {
	VALUE reachable = rb_funcall(rb_mObjectSpace, reachable_objects_from_id, 1, obj);
	long len = RARRAY_LEN(reachable);
	
	for (long i = 0; i < len; i++) {
		VALUE child = rb_ary_entry(reachable, i);

		// Skip internal objects:
		if (rb_obj_is_kind_of(child, rb_cInternalObjectWrapper)) {
			continue;
		}

		func(child, data);
	}
}
#endif

// Context for BFS traversal
struct Memory_Profiler_Graph_Traverse_Context {
	VALUE queue;           // Array serving as BFS queue
	VALUE parents_hash;    // Ruby Hash (compare_by_identity) for parents
	VALUE names_hash;      // Ruby Hash (compare_by_identity) for names
	st_table *visited;     // C hash table for visited tracking (faster lookup)
	VALUE graph_object;    // The Graph instance for calling extract_names!
	VALUE current_parent;  // Current parent being processed (for callback)
};

// Callback for adding reachable objects to queue
static void Memory_Profiler_Graph_traverse_callback(VALUE object, void *data) {
	struct Memory_Profiler_Graph_Traverse_Context *context = (struct Memory_Profiler_Graph_Traverse_Context *)data;
	
	// Skip if already visited
	VALUE object_id = rb_obj_id(object);
	if (st_lookup(context->visited, (st_data_t)object_id, NULL)) {
		return;
	}
	
	// Add to queue with current_parent
	rb_ary_push(context->queue, rb_ary_new_from_args(2, object, context->current_parent));
}

// Graph#traverse!(from) - C implementation of traverse!
static VALUE Memory_Profiler_Graph_traverse(VALUE self, VALUE from) {
	// Get the hashes from the Graph instance
	VALUE parents_hash = rb_ivar_get(self, rb_intern("@parents"));
	VALUE names_hash = rb_ivar_get(self, rb_intern("@names"));
	
	// Clear them
	rb_funcall(parents_hash, rb_intern("clear"), 0);
	rb_funcall(names_hash, rb_intern("clear"), 0);
	
	// Setup context
	struct Memory_Profiler_Graph_Traverse_Context context;
	context.queue = rb_ary_new();
	context.parents_hash = parents_hash;
	context.names_hash = names_hash;
	context.visited = st_init_numtable();
	context.graph_object = self;
	context.current_parent = Qnil;
	
	// Start with initial object
	rb_ary_push(context.queue, rb_ary_new_from_args(2, from, Qnil));
	
	// BFS loop
	while (RARRAY_LEN(context.queue) > 0) {
		VALUE item = rb_ary_shift(context.queue);
		VALUE current = rb_ary_entry(item, 0);
		VALUE parent = rb_ary_entry(item, 1);
		
		// Check if already visited
		VALUE current_id = rb_obj_id(current);
		if (st_lookup(context.visited, (st_data_t)current_id, NULL)) {
			continue;
		}
		
		// Mark as visited
		st_insert(context.visited, (st_data_t)current_id, 0);
		
		// Store parent relationship
		if (!NIL_P(parent)) {
			rb_hash_aset(parents_hash, current, parent);
		}
		
		// Extract names - call back to Ruby for now (can optimize later)
		rb_funcall(self, rb_intern("extract_names!"), 1, current);
		
		// Set current as parent for callback
		context.current_parent = current;
		
		// Get reachable objects and add to queue using our wrapper
		// This automatically filters garbage and internal objects
		Memory_Profiler_Graph_reachable_objects_from(current, Memory_Profiler_Graph_traverse_callback, &context);
	}
	
	// Clean up
	st_free_table(context.visited);
	
	return Qnil;
}

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
	rb_define_method(Memory_Profiler_Graph, "traverse!", Memory_Profiler_Graph_traverse, 1);
}
