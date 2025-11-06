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
				@root = nil  # Track the root of traversal for idom
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
			
			# Find retaining roots using immediate dominator algorithm
			# Returns array of hashes with :name, :count, :percentage
			# C implementation in ext/memory/profiler/graph.c
			# def roots
			# 	roots_with_idom
			# end
			
			def name_for(object, seen = Set.new.compare_by_identity)
				return "" if seen.include?(object)
				seen.add(object)
				
				if object.is_a?(Module)
					# Name could be nil if it is anonymous.
					object.name.to_s
				elsif object.is_a?(Object)
					parents = @parents[object]
					
					if parents&.any?
						parents.map do |parent|
							name_for(parent, seen) + compute_edge_label(parent, object)
						end.join(" | ")
					else
						object.class.name.to_s
					end
				else
					object.inspect
				end
			end
			
		private
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
							@names[key] = variable
						end
					end
				end
			end
		end
	end
end

