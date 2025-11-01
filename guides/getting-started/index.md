# Getting Started

This guide will help you get started with Memory::Tracker for tracking memory allocations and detecting memory leaks in your Ruby applications.

## Installation

Add to your Gemfile:

``` ruby
gem "memory-tracker"
```

Or install directly:

``` bash
gem install memory-tracker
```

## Basic Usage - Count Only (Lightweight)

The simplest way to use Memory::Tracker is to track live object counts without detailed call path analysis. This has minimal overhead and is perfect for monitoring memory growth.

``` ruby
require "memory/tracker"

# Create a capture instance
capture = Memory::Tracker::Capture.new

# Add classes to track (count only - minimal overhead)
capture.track(Hash)
capture.track(Array)

# Start capturing
capture.start

# Your code that might leak memory
leaked_hashes = []
1000.times do
	hash = {}
	leaked_hashes << hash  # These are retained
end

100.times do
	hash = {}  # These will be GC'd and automatically removed from count
end

# Get live object count (O(1) lookup - no heap enumeration!)
live_count = capture.count_for(Hash)
puts "Live Hash objects: #{live_count}"  # 1000

# Stop capturing
capture.stop
```

## Advanced Usage - With Call Path Analysis

When you need to understand **where** objects are being allocated, use call path analysis to capture stack traces:

``` ruby
require "memory/tracker"

# Start tracking with detailed call path analysis
Memory::Tracker.track_with_analysis(Hash)

# Your code that might leak memory
run_potentially_leaky_code

# Analyze call paths (automatically built via callback during allocations!)
stats = Memory::Tracker.stats(Hash)

puts "Live objects: #{stats[:live_count]}"
puts "Total allocation sites: #{stats[:total_allocations]}"

stats[:top_paths].each do |path_data|
	puts "\n#{path_data[:count]} allocations from:"
	path_data[:path].each {|frame| puts "  #{frame}"}
end

# Stop tracking
Memory::Tracker.untrack(Hash)
```

## Custom Callback Usage

For advanced use cases, you can provide custom callbacks to process allocations:

``` ruby
# Create your own callback to process allocations
tracker = Memory::Tracker::Instance.new(depth: 10)

# Track with custom callback
capture = tracker.instance_variable_get(:@capture)
capture.track(Hash) do |obj, klass|
	# Callback decides when to capture caller_locations
	locations = caller_locations(1, 5)
	
	# Only log allocations from specific files
	if locations.first.path.include?("app/models")
		puts "Hash allocated in models: #{locations.first}"
	end
end

# Or use the built-in analysis
tracker.track_with_analysis(Array)

# Get counts (always available, even for count-only tracking)
puts "Live Hashes: #{tracker.count(Hash)}"
puts "Live Arrays: #{tracker.count(Array)}"

# Get detailed stats (only for classes tracked with analysis)
if stats = tracker.stats(Array)
	puts "\nArray Analysis:"
	puts "  Live: #{stats[:live_count]}"
	puts "  Total sites: #{stats[:total_allocations]}"
	
	stats[:top_paths].each do |path_data|
		puts "\n  #{path_data[:count]}x from:"
		path_data[:path].each {|frame| puts "    #{frame}"}
	end
end

# Clean up
tracker.stop!
```

## Integration with Monitoring

Here's an example of integrating Memory::Tracker with application monitoring:

``` ruby
# In a Rack middleware or background job
class MemoryMonitor
	def initialize
		@tracker = Memory::Tracker::Instance.new(depth: 10)
		@samples = {}
	end
	
	def start_monitoring(klass, threshold: 100)
		@samples[klass] = { size: 0, increases: 0 }
		sample_class(klass)
	end
	
	def sample_class(klass)
		current_count = ObjectSpace.each_object(klass).count
		sample = @samples[klass]
		
		if current_count > sample[:size] + threshold
			sample[:size] = current_count
			sample[:increases] += 1
			
			# After 10 increases, start detailed tracking
			if sample[:increases] == 10
				@tracker.track(klass)
				puts "Started tracking #{klass} allocations"
			end
			
			# Report if we're tracking
			if @tracker.tracking?(klass)
				stats = @tracker.stats(klass)
				report_leak(klass, stats)
			end
		end
	end
	
	def report_leak(klass, stats)
		puts "MEMORY LEAK DETECTED: #{klass}"
		puts "Total tracked allocations: #{stats[:total_allocations]}"
		
		stats[:top_paths].first(3).each do |path_data|
			puts "\n#{path_data[:count]} allocations from:"
			path_data[:path].each {|frame| puts "  <- #{frame}"}
		end
	end
end
```

## Using Counters Without Analysis

Perfect for monitoring memory growth with minimal overhead:

``` ruby
capture = Memory::Tracker::Capture.new

# Just track counts (no callback = no caller_locations overhead!)
capture.track(Hash)
capture.track(Array)
capture.track(String)

# Start capturing
capture.start

# In background thread or periodic check
loop do
	puts "Live objects:"
	puts "  Hashes: #{capture.count_for(Hash)}"
	puts "  Arrays: #{capture.count_for(Array)}"
	puts "  Strings: #{capture.count_for(String)}"
	sleep 60
end

# When done
capture.stop
```

This is **extremely efficient** - O(1) hash lookup with no heap enumeration and no caller capture overhead!
