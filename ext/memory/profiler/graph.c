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
		
		// Check if already visited (for traversal)
		VALUE current_id = rb_obj_id(current);
		int already_visited = st_lookup(context.visited, (st_data_t)current_id, NULL);
		
		// Store parent relationship (even if visited, to capture all parents)
		if (!NIL_P(parent)) {
			VALUE parent_list = rb_hash_aref(parents_hash, current);
			if (NIL_P(parent_list)) {
				parent_list = rb_ary_new();
				rb_hash_aset(parents_hash, current, parent_list);
			}
			// Only add parent if not already in the list
			VALUE includes = rb_funcall(parent_list, rb_intern("include?"), 1, parent);
			if (!RTEST(includes)) {
				rb_ary_push(parent_list, parent);
			}
		}
		
		// Skip traversal if already visited (but we've recorded the parent above)
		if (already_visited) {
			continue;
		}
		
		// Mark as visited for traversal
		st_insert(context.visited, (st_data_t)current_id, 0);
		
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

// Helper: Get depth of a node in the parent tree
static long Memory_Profiler_Graph_depth(VALUE node, VALUE parents_hash) {
	long depth = 0;
	VALUE current = node;
	st_table *seen = st_init_numtable();
	
	while (1) {
		VALUE parent = rb_hash_aref(parents_hash, current);
		if (NIL_P(parent)) break;
		
		// Cycle detection
		VALUE current_id = rb_obj_id(current);
		if (st_lookup(seen, (st_data_t)current_id, NULL)) break;
		st_insert(seen, (st_data_t)current_id, 0);
		
		depth++;
		current = parent;
		
		if (depth > 10000) break; // Safety limit
	}
	
	st_free_table(seen);
	return depth;
}

// Helper: Find lowest common ancestor (intersection point) in the dominator tree
static VALUE Memory_Profiler_Graph_intersect(VALUE node1, VALUE node2, VALUE parents_hash, VALUE idom_hash) {
	VALUE finger1 = node1;
	VALUE finger2 = node2;
	st_table *seen = st_init_numtable();
	
	int iterations = 0;
	while (!rb_equal(finger1, finger2)) {
		if (++iterations > 10000) break; // Safety
		
		// Prevent infinite loops
		VALUE finger1_id = rb_obj_id(finger1);
		if (st_lookup(seen, (st_data_t)finger1_id, NULL)) break;
		st_insert(seen, (st_data_t)finger1_id, 0);
		
		// Walk up both paths until they meet
		long depth1 = Memory_Profiler_Graph_depth(finger1, parents_hash);
		long depth2 = Memory_Profiler_Graph_depth(finger2, parents_hash);
		
		if (depth1 > depth2) {
			VALUE idom = rb_hash_aref(idom_hash, finger1);
			if (NIL_P(idom)) break;
			finger1 = idom;
		} else if (depth2 > depth1) {
			VALUE idom = rb_hash_aref(idom_hash, finger2);
			if (NIL_P(idom)) break;
			finger2 = idom;
		} else {
			// Same depth, move both up one level
			VALUE idom1 = rb_hash_aref(idom_hash, finger1);
			VALUE idom2 = rb_hash_aref(idom_hash, finger2);
			
			if (NIL_P(idom1) || NIL_P(idom2)) break;
			
			finger1 = idom1;
			finger2 = idom2;
		}
	}
	
	st_free_table(seen);
	return finger1;
}

// Graph#compute_idom - Compute immediate dominators using Cooper-Harvey-Kennedy algorithm
// Returns a Hash mapping node => immediate_dominator
static VALUE Memory_Profiler_Graph_compute_idom(VALUE self) {
	VALUE parents_hash = rb_ivar_get(self, rb_intern("@parents"));
	VALUE root_node = rb_ivar_get(self, rb_intern("@root"));
	VALUE idom_hash = rb_hash_new();
	rb_funcall(idom_hash, rb_intern("compare_by_identity"), 0);
	
	// In the new model, parents_hash already maps child -> [parents] (array)
	// So we can use it directly as the predecessors map
	VALUE predecessors = parents_hash;
	
	// Get all keys
	VALUE keys = rb_funcall(parents_hash, rb_intern("keys"), 0);
	long keys_len = RARRAY_LEN(keys);
	
	// The root of traversal is the dominator root - it dominates itself
	if (!NIL_P(root_node)) {
		rb_hash_aset(idom_hash, root_node, root_node);
	}
	
	// Find all roots (nodes with no parents or empty parent array)
	VALUE roots = rb_ary_new();
	if (!NIL_P(root_node)) {
		rb_ary_push(roots, root_node);
	}
	
	for (long i = 0; i < keys_len; i++) {
		VALUE node = rb_ary_entry(keys, i);
		VALUE parent_list = rb_hash_aref(parents_hash, node);
		
		// Node is a root if it has no parents or empty parent list
		if (NIL_P(parent_list) || RARRAY_LEN(parent_list) == 0) {
			if (!rb_equal(node, root_node)) {
				rb_ary_push(roots, node);
				// Roots dominate themselves
				rb_hash_aset(idom_hash, node, node);
			}
		}
	}
	
	// Iterative dataflow analysis to compute immediate dominators
	int changed = 1;
	int iterations = 0;
	
	while (changed && iterations < 1000) {
		changed = 0;
		iterations++;
		
		// Process all nodes except roots
		for (long i = 0; i < keys_len; i++) {
			VALUE node = rb_ary_entry(keys, i);
			
			// Skip if this is a root
			VALUE is_root = rb_funcall(roots, rb_intern("include?"), 1, node);
			if (RTEST(is_root)) continue;
			
			// Get predecessors of this node
			VALUE preds = rb_hash_aref(predecessors, node);
			if (NIL_P(preds) || RARRAY_LEN(preds) == 0) continue;
			
			// Find first processed predecessor (one that has an idom)
			VALUE new_idom = Qnil;
			long preds_len = RARRAY_LEN(preds);
			for (long j = 0; j < preds_len; j++) {
				VALUE pred = rb_ary_entry(preds, j);
				VALUE pred_idom = rb_hash_aref(idom_hash, pred);
				if (!NIL_P(pred_idom)) {
					new_idom = pred;
					break;
				}
			}
			
			if (NIL_P(new_idom)) continue;
			
			// Intersect with other processed predecessors
			for (long j = 0; j < preds_len; j++) {
				VALUE pred = rb_ary_entry(preds, j);
				if (rb_equal(pred, new_idom)) continue;
				
				VALUE pred_idom = rb_hash_aref(idom_hash, pred);
				if (NIL_P(pred_idom)) continue;
				
				new_idom = Memory_Profiler_Graph_intersect(new_idom, pred, parents_hash, idom_hash);
			}
			
			// Update if changed
			VALUE old_idom = rb_hash_aref(idom_hash, node);
			if (!rb_equal(old_idom, new_idom)) {
				rb_hash_aset(idom_hash, node, new_idom);
				changed = 1;
			}
		}
	}
	
	return idom_hash;
}

// Graph#roots_with_idom - Return roots using immediate dominator algorithm  
// Returns array of hashes with name, count, percentage, and retained_by details
static VALUE Memory_Profiler_Graph_roots_with_idom(VALUE self) {
	VALUE objects_set = rb_ivar_get(self, rb_intern("@objects"));
	VALUE parents_hash = rb_ivar_get(self, rb_intern("@parents"));
	
	// Compute immediate dominators
	VALUE idom_hash = Memory_Profiler_Graph_compute_idom(self);
	
	// Count how many tracked objects each node directly dominates
	VALUE counts_hash = rb_hash_new();
	rb_funcall(counts_hash, rb_intern("compare_by_identity"), 0);
	
	// Also track direct retention: parent => count of tracked objects with it as immediate parent
	VALUE retained_by_hash = rb_hash_new();
	rb_funcall(retained_by_hash, rb_intern("compare_by_identity"), 0);
	
	// Convert Set to Array for iteration
	VALUE objects_array = rb_funcall(objects_set, rb_intern("to_a"), 0);
	long total = RARRAY_LEN(objects_array);
	
	if (total == 0) {
		return rb_ary_new();
	}
	
	// Each tracked object credits its immediate dominator AND all immediate parents
	for (long i = 0; i < total; i++) {
		VALUE object = rb_ary_entry(objects_array, i);
		
		// Credit the dominator (for dominated count)
		VALUE dominator = rb_hash_aref(idom_hash, object);
		if (!NIL_P(dominator)) {
			VALUE current_count = rb_hash_aref(counts_hash, dominator);
			long count = NIL_P(current_count) ? 0 : NUM2LONG(current_count);
			rb_hash_aset(counts_hash, dominator, LONG2NUM(count + 1));
		}
		
		// Credit all immediate parents (for retained_by count)
		VALUE parent_list = rb_hash_aref(parents_hash, object);
		if (!NIL_P(parent_list) && RB_TYPE_P(parent_list, T_ARRAY)) {
			long parents_len = RARRAY_LEN(parent_list);
			for (long j = 0; j < parents_len; j++) {
				VALUE parent = rb_ary_entry(parent_list, j);
				
				VALUE retained_count = rb_hash_aref(retained_by_hash, parent);
				long count = NIL_P(retained_count) ? 0 : NUM2LONG(retained_count);
				rb_hash_aset(retained_by_hash, parent, LONG2NUM(count + 1));
			}
		}
	}
	
	// Build results array
	VALUE results = rb_ary_new();
	VALUE counts_array = rb_funcall(counts_hash, rb_intern("to_a"), 0);
	long counts_len = RARRAY_LEN(counts_array);
	
	for (long i = 0; i < counts_len; i++) {
		VALUE pair = rb_ary_entry(counts_array, i);
		VALUE object = rb_ary_entry(pair, 0);
		VALUE count_val = rb_ary_entry(pair, 1);
		long dominated_count = NUM2LONG(count_val);
		
		// Build result hash
		VALUE result = rb_hash_new();
		
		// Get name by calling Ruby method
		VALUE name = rb_funcall(self, rb_intern("name_for"), 1, object);
		rb_hash_aset(result, ID2SYM(rb_intern("name")), name);
		rb_hash_aset(result, ID2SYM(rb_intern("count")), count_val);
		
		double percentage = (dominated_count * 100.0) / total;
		rb_hash_aset(result, ID2SYM(rb_intern("percentage")), rb_float_new(percentage));
		
		// Add retained_by count (how many have this as immediate parent)
		VALUE retained_count_val = rb_hash_aref(retained_by_hash, object);
		if (!NIL_P(retained_count_val)) {
			rb_hash_aset(result, ID2SYM(rb_intern("retained_by")), retained_count_val);
		}
		
		rb_ary_push(results, result);
	}
	
	// Also add entries that appear ONLY in retained_by but not as dominators
	// These are intermediate nodes in diamond patterns
	VALUE retained_array = rb_funcall(retained_by_hash, rb_intern("to_a"), 0);
	long retained_len = RARRAY_LEN(retained_array);
	
	for (long i = 0; i < retained_len; i++) {
		VALUE pair = rb_ary_entry(retained_array, i);
		VALUE object = rb_ary_entry(pair, 0);
		
		// Skip if already in results (appears in counts_hash as dominator)
		VALUE already_counted = rb_hash_aref(counts_hash, object);
		if (!NIL_P(already_counted)) continue;
		
		long retained_count = NUM2LONG(rb_ary_entry(pair, 1));
		
		VALUE result = rb_hash_new();
		VALUE name = rb_funcall(self, rb_intern("name_for"), 1, object);
		
		rb_hash_aset(result, ID2SYM(rb_intern("name")), name);
		rb_hash_aset(result, ID2SYM(rb_intern("count")), LONG2NUM(0));  // Doesn't dominate
		rb_hash_aset(result, ID2SYM(rb_intern("percentage")), rb_float_new(0.0));
		rb_hash_aset(result, ID2SYM(rb_intern("retained_by")), LONG2NUM(retained_count));
		
		rb_ary_push(results, result);
	}
	
	// Simple bubble sort by count (descending)
	long results_len = RARRAY_LEN(results);
	for (long i = 0; i < results_len - 1; i++) {
		for (long j = 0; j < results_len - i - 1; j++) {
			VALUE item1 = rb_ary_entry(results, j);
			VALUE item2 = rb_ary_entry(results, j + 1);
			
			long count1 = NUM2LONG(rb_hash_aref(item1, ID2SYM(rb_intern("count"))));
			long count2 = NUM2LONG(rb_hash_aref(item2, ID2SYM(rb_intern("count"))));
			
			if (count1 < count2) {
				// Swap
				rb_ary_store(results, j, item2);
				rb_ary_store(results, j + 1, item1);
			}
		}
	}
	
	return results;
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
	rb_define_method(Memory_Profiler_Graph, "compute_idom", Memory_Profiler_Graph_compute_idom, 0);
	rb_define_method(Memory_Profiler_Graph, "roots", Memory_Profiler_Graph_roots_with_idom, 0);
}
