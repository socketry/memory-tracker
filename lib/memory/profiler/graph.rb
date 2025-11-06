# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "objspace"
require_relative "native"

module Memory
	module Profiler
		class Graph
			def initialize
				@objects = Set.new.compare_by_identity
				@parents = Hash.new{|hash, key| hash[key] = Set.new.compare_by_identity}.compare_by_identity
				@names = Hash.new.compare_by_identity
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
				
				# If the user has provided a specific object, try to avoid traversing the root Object.
				if from != Object
					@parents.clear
					@names.clear
					@parents[Object] = nil
				end
				
				traverse!(from)
			end
			
			def name_for(object, seen = Set.new.compare_by_identity)
				return "" if seen.include?(object)
				seen.add(object)
				
				if object.is_a?(Module)
					# Name could be nil if it is anonymous.
					object.name.to_s
				elsif object.is_a?(Object)
					parents = @parents[object]
					
					if parents&.any?
						names = parents.map do |parent|
							name_for(parent, seen.dup) + compute_edge_label(parent, object)
						end
						
						if names.size > 1
							return "[#{names.join("|")}]"
						else
							names.first
						end
					else
						object.class.name.to_s
					end
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
			
			def traverse!(object, parent = nil)
				queue = Array.new
				queue << [object, parent]
				
				while queue.any?
					object, parent = queue.shift
					extract_names!(object)
					
					reachable_objects_from(object) do |child|
						next unless parents = @parents[child]
						if parents.add?(object)
							queue << [child, object]
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
						key = parent.key(object)
						return key ? "[#{key.inspect}]" : object.class.name
					else
						return "[#{parent.size} keys]"
					end
				when Array
					if parent.size < SEARCH_LIMIT
						# Use Ruby's built-in index method to find the position
						index = parent.index(object)
						return index ? "[#{index}]" : object.class.name
					else
						return "[#{parent.size} elements]"
					end
				when Object
					return @names[object]&.to_s || object.class.name
				else
					return "(unknown)"
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
					return finger1 if iterations > SAFETY_LIMIT
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
			
			IS_A = Kernel.method(:is_a?).unbind
			
			def extract_names!(from)
				if IS_A.bind_call(from, Module)
					from.constants.each do |constant|
						if !from.autoload?(constant) && from.const_defined?(constant)
							if key = from.const_get(constant) and IS_A.bind_call(key, BasicObject)
								@names[key] = "::#{constant}"
							end
						end
					end
				elsif IS_A.bind_call(from, Object)
					from.instance_variables.each do |variable|
						if key = from.instance_variable_get(variable) and IS_A.bind_call(key, BasicObject)
							@names[key] = ".#{variable}"
						end
					end
				end
			end
		end
	end
end

