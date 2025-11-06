# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "objspace"

module Memory
	module Profiler
		class Graph
			def initialize
				@objects = Set.new.compare_by_identity
				@parents = Hash.new{|hash, key| hash[key] = Set.new.compare_by_identity}.compare_by_identity
				@internals = Set.new
				@root = nil # Track the root of traversal for idom
			end
			
			attr :parents
			
			def include?(object)
				@parents.include?(object)
			end
			
			def inspect
				"#<#{self.class.name} @objects=#{@objects.size} @parents=#{@parents.size} @names=#{@names.size}>"
			end
			
			def add(object)
				@objects.add(object)
			end
			
			def update!(from = Object)
				# Store the root for idom algorithm
				@root = from
				@parents.clear
				@internals.clear

				# If the user has provided a specific object, try to avoid traversing the root Object.
				if from != Object
					@parents[Object] = nil
				end
				
				# Don't traverse the graph itself:
				@parents[self] = nil
				
				traverse!(from)
			end
			
			def name_for(object, limit = 8)
				if object.is_a?(Module)
					object.name.to_s
				elsif object.is_a?(Object)
					if limit > 0
						parents = @parents[object]
						if parent = parents&.first
							return name_for(parent, limit - 1) + compute_edge_label(parent, object)
						end
					end
					
					return object.class.name.to_s
				else
					object.inspect
				end
			end
			
			# Return roots using immediate dominator algorithm
			def roots
				return [] if @objects.empty?
				
				# Compute immediate dominators
				idom = compute_idom
				
				# Count how many tracked objects each node dominates
				dominated_counts = Hash.new(0).compare_by_identity
				retained_by_counts = Hash.new(0).compare_by_identity
				
				@objects.each do |object|
					# Credit the immediate dominator
					if dominator = idom[object]
						dominated_counts[dominator] += 1
					end
					
					# Credit all immediate parents for retained_by
					if parent_set = @parents[object]
						parent_set.each do |parent|
							retained_by_counts[parent] += 1
						end
					end
				end
				
				# Build results
				total = @objects.size
				results = []
				
				dominated_counts.each do |object, count|
					result = {
						name: name_for(object),
							count: count,
							percentage: (count * 100.0) / total
					}
					
					if retained_count = retained_by_counts[object]
						result[:retained_by] = retained_count
					end
					
					results << result
				end
				
				# Add entries that appear ONLY in retained_by (intermediate nodes)
				retained_by_counts.each do |object, count|
					next if dominated_counts[object]  # Already included
					
					results << {
						name: name_for(object),
						count: 0,
						percentage: 0.0,
						retained_by: count
					}
				end
				
				# Sort by count descending
				results.sort_by!{|r| -r[:count]}
				
				results
			end
			
		private

			IS_A = Kernel.method(:is_a?).unbind
			RESPOND_TO = Kernel.method(:respond_to?).unbind
			EQUAL = Kernel.method(:equal?).unbind
			
			def traverse!(object, parent = nil)
				queue = Array.new
				queue << [object, parent]
				
				while queue.any?
					object, parent = queue.shift
					
					# We shouldn't use internal objects as parents.
					unless IS_A.bind_call(object, ObjectSpace::InternalObjectWrapper)
						parent = object
					end
					
					ObjectSpace.reachable_objects_from(object).each do |child|
						if IS_A.bind_call(child, ObjectSpace::InternalObjectWrapper)
							# There is no value in scanning internal objects.
							next if child.type == :T_IMEMO

							# We need to handle internal objects differently, because they don't follow the same equality/identity rules. Since we can reach the same internal object from multiple parents, we need to use an appropriate key to track it. Otherwise, the objects will not be counted on subsequent parent traversals.
							key = [parent.object_id, child.internal_object_id]
							if @internals.add?(key)
								queue << [child, parent]
							end
						elsif parents = @parents[child] # Skip traversal if we are explicitly set to nil.
							# If we haven't seen the object yet, add it to the queue:
							if parents.add?(parent)
								queue << [child, parent]
							end
						end
					end
				end
			end
			
			# Limit the cost of computing the edge label.
			SEARCH_LIMIT = 1000
			
			# Lazily compute the edge label (how parent references child)
			# This is called on-demand for objects that appear in roots
			def compute_edge_label(parent, object)
				case parent
				when Hash
					if parent.size < SEARCH_LIMIT
						# Use Ruby's built-in key method to find the key
						if key = parent.key(object)
							return "[#{key.inspect}]"
						end
					end
					return "[?/#{parent.size}]"
				when Array
					if parent.size < SEARCH_LIMIT
						# Use Ruby's built-in index method to find the position
						if index = parent.index(object)
							return "[#{index}]"
						end
					end
					return "[?/#{parent.size}]"
				else
					return extract_name(parent, object)
				end
			rescue => error
				# If inspection fails, fall back to class name
				return "(#{error.class.name}: #{error.message})"
			end
			
			# Compute immediate dominators for all nodes
			# Returns hash mapping node => immediate dominator
			def compute_idom
				idom = Hash.new.compare_by_identity
				
				# Root dominates itself
				idom[@root] = @root if @root
				
				# Find all roots (nodes with no parents)
				roots = Set.new.compare_by_identity
				roots.add(@root) if @root
				
				# Convert to array to avoid "can't add key during iteration" errors
				@parents.each do |node, parents|
					if parents.nil? || parents.empty?
						roots.add(node)
						idom[node] = node
					end
				end
				
				# Iterative dataflow analysis
				changed = true
				iterations = 0
				
				while changed && iterations < 1000
					changed = false
					iterations += 1
					
					# Convert to array to avoid "can't add key during iteration" errors
					@parents.each do |node, parents|
						# Skip roots:
						next if roots.include?(node)
						next if parents.nil? || parents.empty?
						
						# Find first processed predecessor
						new_idom = parents.find{|parent| idom[parent]}
						next unless new_idom
						
						# Intersect with other processed predecessors
						parents.each do |parent|
							next if parent.equal?(new_idom)
							next unless idom[parent]
							
							new_idom = intersect(new_idom, parent, idom)
						end
						
						# Update if changed
						if !idom[node].equal?(new_idom)
							idom[node] = new_idom
							changed = true
						end
					end
				end
				
				idom
			end
			
			# Find lowest common ancestor in dominator tree
			def intersect(node1, node2, idom)
				finger1 = node1
				finger2 = node2
				seen = Set.new.compare_by_identity
				iterations = 0
				
				until finger1.equal?(finger2)
					return finger1 if iterations > SEARCH_LIMIT
					iterations += 1
					
					# Prevent infinite loops
					return finger1 if seen.include?(finger1)
					seen.add(finger1)
					
					# Walk up both paths until they meet
					depth1 = depth(finger1)
					depth2 = depth(finger2)
					
					if depth1 > depth2
						break unless idom[finger1]
						finger1 = idom[finger1]
					elsif depth2 > depth1
						break unless idom[finger2]
						finger2 = idom[finger2]
					else
						# Same depth, walk both up
						break unless idom[finger1] && idom[finger2]
						finger1 = idom[finger1]
						finger2 = idom[finger2]
					end
				end
				
				finger1
			end
			
			# Compute depth of node in parent tree
			def depth(node)
				depth = 0
				current = node
				seen = Set.new.compare_by_identity
				
				while current
					return depth if seen.include?(current)
					seen.add(current)
					
					break unless @parents.key?(current)
					parents = @parents[current]
					break if parents.empty?
					
					current = parents.first # TODO: Should we use the first parent?
					depth += 1
				end
				
				depth
			end
			
			def extract_name(parent, object)
				if RESPOND_TO.bind_call(parent, :constants)
					parent.constants.each do |constant|
						if !parent.autoload?(constant) && parent.const_defined?(constant)
							if EQUAL.bind_call(parent.const_get(constant), object)
								return "::#{constant}"
							end
						end
					end
				end
				
				if RESPOND_TO.bind_call(parent, :instance_variables)
					parent.instance_variables.each do |variable|
						if EQUAL.bind_call(parent.instance_variable_get(variable), object)
							return ".#{variable}"
						end
					end
				end
				
				if IS_A.bind_call(parent, Struct)
					parent.members.each do |member|
						if EQUAL.bind_call(parent[member], object)
							return ".#{member}"
						end
					end
				end
				
				if IS_A.bind_call(parent, Fiber)
					return "(fiber)"
				end
				
				if IS_A.bind_call(parent, Thread)
					parent.thread_variables.each do |variable|
						if EQUAL.bind_call(parent.thread_variable_get(variable), object)
							return "(TLS #{variable})"
						end
					end
					
					parent.keys.each do |key|
						if EQUAL.bind_call(parent[key], object)
							return "[#{key}]"
						end
					end
					
					return "(thread)"
				end
				
				if IS_A.bind_call(parent, Ractor)
					return "(ractor)"
				end
				
				return "(#{parent.inspect}::???)"
			rescue => error
				# If inspection fails, fall back to class name
				return "(#{error.class.name}: #{error.message})"
			end
		end
	end
end

