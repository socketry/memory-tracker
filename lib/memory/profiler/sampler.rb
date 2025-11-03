# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "console"

require_relative "capture"
require_relative "call_tree"

module Memory
	module Profiler
		# Periodic sampler for monitoring memory growth over time.
		#
		# Samples class allocation counts at regular intervals and detects potential memory leaks
		# by tracking when counts increase beyond a threshold. When a class exceeds the increases
		# threshold, automatically enables detailed call path tracking for diagnosis.
		class Sampler
			# Tracks memory growth for a specific class.
			#
			# Records allocation counts over time and detects sustained growth patterns
			# that indicate potential memory leaks.
			class Sample
				# Create a new sample profiler for a class.
				#
				# @parameter target [Class] The class being sampled.
				# @parameter size [Integer] Initial object count.
				# @parameter threshold [Integer] Minimum increase to consider significant.
				def initialize(target, size = 0, threshold: 1000)
					@target = target
					@current_size = size
					@maximum_observed_size = size
					@threshold = threshold
					
					@sample_count = 0
					@increases = 0
				end
				
				attr_reader :target, :current_size, :maximum_observed_size, :threshold, :sample_count, :increases
				
				# Record a new sample measurement.
				#
				# @parameter size [Integer] Current object count for this class.
				# @returns [Boolean] True if count increased significantly.
				def sample!(size)
					@sample_count += 1
					@current_size = size
					
					# @maximum_observed_count ratchets up in units of at least @threshold counts.
					# When it does, we bump @increases to track a potential memory leak.
					if @maximum_observed_size
						delta = @current_size - @maximum_observed_size
						if delta > @threshold
							@maximum_observed_size = size
							@increases += 1
							
							return true
						end
					else
						@maximum_observed_size = size
					end
					
					return false
				end
				
				# Convert sample data to JSON-compatible hash.
				#
				# @returns [Hash] Sample data as a hash.
				def as_json(...)
					{
						target: @target.name || "(anonymous class)",
						current_size: @current_size,
						maximum_observed_size: @maximum_observed_size,
						increases: @increases,
						sample_count: @sample_count,
						threshold: @threshold,
					}
				end
				
				# Convert sample data to JSON string.
				#
				# @returns [String] Sample data as JSON.
				def to_json(...)
					as_json.to_json(...)
				end
			end
			
			# Create a new memory sampler.
			#
			# @parameter depth [Integer] Number of stack frames to capture for call path analysis.
			# @parameter filter [Proc] Optional filter to exclude frames from call paths.
			# @parameter increases_threshold [Integer] Number of increases before enabling detailed tracking.
			# @parameter prune_limit [Integer] Keep only top N children per node during pruning (default: 5).
			# @parameter prune_threshold [Integer] Number of insertions before auto-pruning (nil = no auto-pruning).
			def initialize(depth: 4, filter: nil, increases_threshold: 10, prune_limit: 5, prune_threshold: nil)
				@depth = depth
				@filter = filter || default_filter
				@increases_threshold = increases_threshold
				@prune_limit = prune_limit
				@prune_threshold = prune_threshold
				@capture = Capture.new
				@call_trees = {}
				@samples = {}
			end

			# @attribute [Integer] The depth of the call tree.
			attr :depth
			
			# @attribute [Proc] The filter to exclude frames from call paths.
			attr :filter

			# @attribute [Integer] The number of increases before enabling detailed tracking.
			attr :increases_threshold

			# @attribute [Integer] The number of insertions before auto-pruning (nil = no auto-pruning).
			attr :prune_limit

			# @attribute [Integer | Nil] The number of insertions before auto-pruning (nil = no auto-pruning).
			attr :prune_threshold

			# @attribute [Capture] The capture object.
			attr :capture

			# @attribute [Hash] The call trees.
			attr :call_trees

			# @attribute [Hash] The samples for each class being tracked.
			attr :samples
			
			# Start capturing allocations.
			def start
				@capture.start
			end
			
			# Stop capturing allocations.
			def stop
				@capture.stop
			end
			
			# Run periodic sampling in a loop.
			#
			# Samples allocation counts at the specified interval and reports when
			# classes show sustained memory growth. Automatically tracks ALL classes
			# that allocate objects - no need to specify them upfront.
			#
			# @parameter interval [Numeric] Seconds between samples.
			# @yields {|sample| ...} Called when a class shows significant growth.
			def run(interval: 60, &block)
				start_time = Process.clock_gettime(Process::CLOCK_MONOTONIC)
				
				while true
					sample!(&block)
					
					now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
					delta = interval - (now - start_time)
					sleep(delta) if delta > 0
					start_time = now
				end
			end
			
			# Take a single sample of memory usage for all tracked classes.
			#
			# @yields {|sample| ...} Called when a class shows significant growth.
			def sample!
				@capture.each do |klass, allocations|
					count = allocations.retained_count
					sample = @samples[klass] ||= Sample.new(klass, count)
					
					if sample.sample!(count)
						# Check if we should enable detailed tracking
						if sample.increases >= @increases_threshold && !@call_trees.key?(klass)
							track(klass, allocations)
						end
						
						# Notify about growth if block given
						yield sample if block_given?
					end
				end
				
				# Prune call trees to control memory usage
				prune_call_trees!
			end
			
			# Start tracking with call path analysis.
			#
			# @parameter klass [Class] The class to track with detailed analysis.
			def track(klass, allocations = nil)
				# Track the class and get the allocations object
				allocations ||= @capture.track(klass)
				
				# Set up call tree for this class
				tree = @call_trees[klass] = CallTree.new
				depth = @depth
				filter = @filter
				
				# Register callback on allocations object:
				# - On :newobj - returns state (leaf node) which C extension stores
				# - On :freeobj - receives state back from C extension
				allocations.track do |klass, event, state|
					case event
					when :newobj
						# Capture call stack and record in tree
						locations = caller_locations(1, depth)
						filtered = locations.select(&filter)
						unless filtered.empty?
							# Record returns the leaf node - return it so C can store it:
							tree.record(filtered)
						end
						# Return nil or the node - C will store whatever we return.
					when :freeobj
						# Decrement using the state (leaf node) passed back from then native extension:
						state&.decrement_path!
					end
				rescue Exception => error
					warn "Error in allocation tracking: #{error.message}\n#{error.backtrace.join("\n")}"
				end
			end
			
			# Stop tracking a specific class.
			def untrack(klass)
				@capture.untrack(klass)
				@call_trees.delete(klass)
			end
			
			# Check if a class is being tracked.
			def tracking?(klass)
				@capture.tracking?(klass)
			end
			
			# Get live object count for a class.
			def count(klass)
				@capture.count_for(klass)
			end
			
			# Get the call tree for a specific class.
			def call_tree(klass)
				@call_trees[klass]
			end
			
			# Get allocation statistics for a tracked class.
			#
			# @parameter klass [Class] The class to get statistics for.
			# @returns [Hash] Statistics including total, retained, paths, and hotspots.
			def statistics(klass)
				tree = @call_trees[klass]
				return nil unless tree
				
				{
					live_count: @capture.count_for(klass),
					total_allocations: tree.total_allocations,
					retained_allocations: tree.retained_allocations,
					top_paths: tree.top_paths(10).map {|path, total, retained| 
						{ path: path, total_count: total, retained_count: retained }
					},
					hotspots: tree.hotspots(20).transform_values {|total, retained|
						{ total_count: total, retained_count: retained }
					}
				}
			end
			
			# Get statistics for all tracked classes.
			def all_statistics
				@call_trees.keys.each_with_object({}) do |klass, result|
					result[klass] = statistics(klass) if tracking?(klass)
				end
			end
			
			# Clear tracking data for a class.
			def clear(klass)
				tree = @call_trees[klass]
				tree&.clear!
			end
			
			# Clear all tracking data.
			def clear_all!
				@call_trees.each_value(&:clear!)
				@capture.clear
			end
			
			# Stop all tracking and clean up.
			def stop!
				@capture.stop
				@call_trees.each_key do |klass|
					@capture.untrack(klass)
				end
				@capture.clear
				@call_trees.clear
			end
			
		private
			
			def default_filter
				->(location) {!location.path.match?(%r{/(gems|ruby)/|\A\(eval\)})}
			end
			
			def prune_call_trees!
				return if @prune_threshold.nil?
				
				@call_trees.each do |klass, tree|
					# Only prune if insertions exceed threshold:
					insertions = tree.insertion_count
					next if insertions < @prune_threshold
					
					# Prune the tree
					pruned_count = tree.prune!(@prune_limit)
					
					# Reset insertion counter after pruning
					tree.insertion_count = 0
					
					# Log pruning activity for visibility
					if pruned_count > 0 && defined?(Console)
						Console.debug(klass, "Pruned call tree:",
							pruned_nodes: pruned_count,
							insertions_since_last_prune: insertions,
							total: tree.total_allocations,
							retained: tree.retained_allocations
						)
					end
				end
			end
		end
	end
end

