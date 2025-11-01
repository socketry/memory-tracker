# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

module Memory
	module Tracker
		# Efficient tree structure for tracking allocation call paths.
		# Each node represents a frame in the call stack, with counts of how many
		# allocations occurred at this point in the call path.
		class CallTree
			# Represents a node in the call tree.
			#
			# Each node tracks how many allocations occurred at a specific point in a call path.
			# Nodes form a tree structure where each path from root to leaf represents a unique
			# call stack that led to allocations.
			class Node
				# Create a new call tree node.
				#
				# @parameter location [Thread::Backtrace::Location] The source location for this frame.
				def initialize(location = nil)
					@location = location
					@count = 0
					@children = nil
				end
				
				# @attribute [Thread::Backtrace::Location] The location of the call.
				attr_reader :location, :count, :children
				
				# Increment the allocation count for this node.
				def increment!
					@count += 1
				end
				
				# Check if this node is a leaf (end of a call path).
				#
				# @returns [Boolean] True if this node has no children.
				def leaf?
					@children.nil?
				end
				
				# Find or create a child node for the given location.
				#
				# @parameter location [Thread::Backtrace::Location] The frame location for the child node.
				# @returns [Node] The child node for this location.
				def find_or_create_child(location)
					@children ||= {}
					@children[location.to_s] ||= Node.new(location)
				end
				
				# Iterate over child nodes.
				#
				# @yields {|child| ...} If a block is given, yields each child node.
				def each_child(&block)
					@children&.each_value(&block)
				end
				
				# Enumerate all paths from this node to leaves with their counts
				def each_path(prefix = [], &block)
					current = prefix + [self]
					
					if leaf?
						yield current, @count
					end
					
					@children&.each_value do |child|
						child.each_path(current, &block)
					end
				end
			end
			
			# Create a new call tree for tracking allocation paths.
			def initialize
				@root = Node.new
			end
			
			# Record an allocation with the given caller locations
			def record(caller_locations)
				return if caller_locations.empty?
				
				current = @root
				
				# Build tree, incrementing each node as we traverse:
				caller_locations.each do |location|
					current.increment!
					current = current.find_or_create_child(location)
				end
				
				# Increment the final leaf node:
				current.increment!
			end
			
			# Get the top N call paths by allocation count
			def top_paths(limit = 10)
				paths = []
				
				@root.each_path do |path, count|
					# Filter out root node (has nil location) and map to location strings
					locations = path.select(&:location).map {|node| node.location.to_s}
					paths << [locations, count] unless locations.empty?
				end
				
				paths.sort_by {|_, count| -count}.first(limit)
			end
			
			# Get hotspot locations (individual frames with highest counts)
			def hotspots(limit = 20)
				frames = Hash.new(0)
				
				collect_frames(@root, frames)
				
				frames.sort_by {|_, count| -count}.first(limit).to_h
			end
			
			# Total number of allocations tracked
			def total_allocations
				@root.instance_variable_get(:@count)
			end
			
			# Clear all tracking data
			def clear!
				@root = Node.new
			end
			
			private
			
			def collect_frames(node, frames)
				# Skip root node (has no location)
				if node.location
					frames[node.location.to_s] += node.count
				end
				
				node.each_child {|child| collect_frames(child, frames)}
			end
		end
	end
end

