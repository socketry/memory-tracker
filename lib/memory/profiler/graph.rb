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
				@parents = Hash.new.compare_by_identity
				@names = Hash.new.compare_by_identity
			end
			
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
				# If the user has provided a specific object, try to avoid traversing the root Object.
				if from != Object
					@parents.clear
					@names.clear
					@parents[Object] = nil
				end
				
				traverse!(from)
			end
			
			def roots
				counts = Hash.new(0).compare_by_identity
				
				@objects.each do |object|
					seen = Set.new.compare_by_identity
					while object = @parents[object]
						break if seen.include?(object)
						counts[object] += 1
						seen.add(object)
					end
				end
				
				counts.sort_by{|_, count| -count}.map do |object, count|
					{
						name: name_for(object),
							count: count,
							percentage: count.to_f / @objects.size * 100
					}
				end
			end
			
			def name_for(object, seen = Set.new.compare_by_identity)
				return "" if seen.include?(object)
				seen.add(object)
				
				if object.is_a?(Module)
					object.name
				elsif object.is_a?(Object)
					name = @names.fetch(object){object.class.name}
					
					if parent = @parents[object]
						name_for(parent, seen) + name.to_s
					else
						name.to_s
					end
				else
					object.inspect
				end
			end
			
	private
			
			# Ruby implementation (commented out, replaced by traverse_c! in C extension)
			# def traverse!(from, parent = nil)
			# 	queue = [[from, parent]]
			# 	
			# 	while item = queue.shift
			# 		current, parent = item
			# 		
			# 		# Store parent relationship:
			# 		if @parents.include?(current)
			# 			next # Already visited.
			# 		elsif parent
			# 			@parents[current] = parent
			# 		end
			# 		
			# 		# Extract names:
			# 		extract_names!(current)
			# 		
			# 		# Queue reachable objects
			# 		ObjectSpace.reachable_objects_from(current).each do |object|
			# 			# These will cause infinite recursion:
			# 			next if object.is_a?(ObjectSpace::InternalObjectWrapper)
			# 			
			# 			# Already visited:
			# 			next if @parents.key?(object)
			# 			
			# 			# Add to queue:
			# 			queue << [object, current]
			# 		end
			# 	end
			# end
			
			# Called from C traverse! implementation
			def extract_names!(from)
				if from.is_a?(Module)
					from.constants.each do |constant|
						if !from.autoload?(constant) && from.const_defined?(constant)
							key = from.const_get(constant)
							@names[key] = "::#{constant}" if key&.is_a?(Object)
						end
					end
				elsif from.is_a?(Object)
					from.instance_variables.each do |variable|
						key = from.instance_variable_get(variable)
						@names[key] = variable if key&.is_a?(Object)
					end
				end
			end
		end
	end
end

