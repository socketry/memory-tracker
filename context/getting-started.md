# Getting Started

This guide explains how to use `memory-profiler` to automatically detect and diagnose memory leaks in Ruby applications.

## Installation

Add the gem to your project:

~~~ bash
$ bundle add memory-profiler
~~~

## Core Concepts

Memory leaks happen when your application creates objects that should be garbage collected but remain referenced indefinitely. Over time, this causes unbounded memory growth, leading to performance degradation or crashes.

- {ruby Memory::Profiler::Capture} monitors allocations using Ruby's internal NEWOBJ/FREEOBJ events.
- {ruby Memory::Profiler::CallTree} aggregates allocation call paths to identify leak sources.
- **No heap enumeration** - uses O(1) counters updated automatically by the VM.

## Basic Usage

The simplest way to detect memory leaks is to run the automatic sampler:

~~~ ruby
require 'memory/profiler'

# Create a sampler that monitors all allocations:
sampler = Memory::Profiler::Sampler.new(
	# Call stack depth for analysis:
	depth: 10,

	# Enable detailed tracking after 10 increases:
	increases_threshold: 10
)

sampler.start

# Run periodic sampling in a background thread:
Thread.new do
	sampler.run(interval: 60) do |sample|
		puts "⚠️  #{sample.target} growing: #{sample.current_size} objects (#{sample.increases} increases)"
		
		# After 10 increases, detailed statistics are automatically available:
		if sample.increases >= 10
			statistics = sampler.statistics(sample.target)
			puts "Top leak sources:"
			statistics[:top_paths].each do |path_data|
				puts "  #{path_data[:count]}x from: #{path_data[:path].first}"
			end
		end
	end
end

# Your application runs here...
objects = []
while true
	# Simulate a memory leak:
	objects << Hash.new
	sleep 0.1
end
~~~

**What happens:**
1. Sampler automatically tracks every class that allocates objects.
2. Every 60 seconds, checks if any class grew significantly (>1000 objects).
3. Reports growth via the block you provide.
4. After 10 sustained increases, automatically captures call paths.
5. You can then query `statistics(klass)` to find leak sources.

## Manual Investigation

If you already know which class is leaking, you can investigate immediately:

~~~ ruby
sampler = Memory::Profiler::Sampler.new(depth: 15)
sampler.start

# Enable detailed tracking for specific class:
sampler.track_with_analysis(Hash)

# Run code that triggers the leak:
1000.times { process_request }

# Analyze:
statistics = sampler.statistics(Hash)

puts "Live Hashes: #{statistics[:live_count]}"
puts "\nTop allocation sources:"
statistics[:top_paths].first(5).each do |path_data|
	puts "\n#{path_data[:count]} allocations from:"
	path_data[:path].each { |frame| puts "  #{frame}" }
end

puts "\nHotspot frames:"
statistics[:hotspots].first(5).each do |location, count|
	puts "  #{location}: #{count}"
end

sampler.stop!
~~~

## Understanding the Output

**Sample data** (from growth detection):
- `target`: The class showing growth
- `current_size`: Current live object count
- `increases`: Number of sustained growth events (>1000 objects each)
- `threshold`: Minimum growth to trigger an increase

**Statistics** (after detailed tracking enabled):
- `live_count`: Current retained objects
- `top_paths`: Complete call stacks ranked by allocation frequency
- `hotspots`: Individual frames aggregated across all paths

**Top paths** show WHERE objects are created:
```
50 allocations from:
	app/services/processor.rb:45:in 'process_item'
	app/workers/job.rb:23:in 'perform'
```

**Hotspots** show which lines appear most across all paths:
```
app/services/processor.rb:45: 150    ← This line in many different call stacks
```

## Performance Considerations

**Automatic mode** (recommended for production):
- Minimal overhead initially (just counting).
- Detailed tracking only enabled when leaks detected.
- 60-second sampling interval is non-intrusive.

**Manual tracking** (for investigation):
- Higher overhead (captures `caller_locations` on every allocation).
- Use during debugging, not continuous monitoring.
- Only track specific classes you're investigating.
