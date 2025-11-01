# Getting Started

This guide explains how to use `memory-tracker` to detect and diagnose memory leaks in Ruby applications.

## Installation

Add the gem to your project:

~~~ bash
$ bundle add memory-tracker
~~~

## Core Concepts

Memory leaks happen when your application creates objects that should be garbage collected but remain referenced indefinitely. Over time, this causes memory usage to grow unbounded, eventually leading to performance degradation or out-of-memory crashes.

`memory-tracker` helps you find memory leaks by tracking object allocations in real-time:

- **{ruby Memory::Tracker::Capture}** monitors allocations using Ruby's internal NEWOBJ/FREEOBJ events.
- **{ruby Memory::Tracker::CallTree}** aggregates allocation call paths to identify leak sources.
- **No heap enumeration** - uses O(1) counters updated automatically by the VM.

## Usage

### Monitor Memory Growth

Start by identifying which classes are accumulating objects:

~~~ ruby
require 'memory/tracker'

# Create a capture instance:
capture = Memory::Tracker::Capture.new

# Start tracking all object allocations:
capture.start

# Run your application code...
run_your_app

# Check live object counts for common classes:
puts "Hashes: #{capture.count_for(Hash)}"
puts "Arrays: #{capture.count_for(Array)}"
puts "Strings: #{capture.count_for(String)}"

capture.stop
~~~

**What this tells you**: Which object types are growing over time. If Hash count keeps increasing across multiple samples, you likely have a Hash leak.

### Find the Leak Source

Once you've identified a leaking class, use call path analysis to find WHERE allocations come from:

~~~ ruby
# Create a sampler with call path analysis:
sampler = Memory::Tracker::Sampler.new(depth: 10)

# Track the leaking class with analysis:
sampler.track_with_analysis(Hash)
sampler.start

# Run code that triggers the leak:
simulate_leak

# Analyze where allocations come from:
statistics = sampler.statistics(Hash)

puts "Live objects: #{statistics[:live_count]}"
puts "\nTop allocation sources:"
statistics[:top_paths].first(5).each do |path_data|
  puts "\n#{path_data[:count]} allocations from:"
  path_data[:path].each { |frame| puts "  #{frame}" }
end

sampler.stop
~~~

**What this shows**: The complete call stacks that led to Hash allocations. Look for unexpected paths or paths that appear repeatedly.

## Real-World Example

Let's say you notice your app's memory growing over time. Here's how to diagnose it:

~~~ ruby
require 'memory/tracker'

# Setup monitoring:
capture = Memory::Tracker::Capture.new
capture.start

# Take baseline measurement:
GC.start  # Clean up old objects first
baseline = {
  hashes: capture.count_for(Hash),
  arrays: capture.count_for(Array),
  strings: capture.count_for(String)
}

# Run your application for a period:
# In production: sample periodically (every 60 seconds)
# In development: run through typical workflows
sleep 60

# Check what grew:
current = {
  hashes: capture.count_for(Hash),
  arrays: capture.count_for(Array),
  strings: capture.count_for(String)
}

# Report growth:
current.each do |type, count|
  growth = count - baseline[type]
  if growth > 100
    puts "⚠️  #{type} grew by #{growth} objects"
  end
end

capture.stop
~~~

If Hashes grew significantly, enable detailed tracking:

~~~ ruby
# Create detailed sampler:
sampler = Memory::Tracker::Sampler.new(depth: 15)
sampler.track_with_analysis(Hash)
sampler.start

# Run suspicious code path:
process_user_requests(1000)

# Find the culprits:
statistics = sampler.statistics(Hash)
statistics[:top_paths].first(3).each_with_index do |path_data, i|
  puts "\n#{i+1}. #{path_data[:count]} Hash allocations:"
  path_data[:path].first(5).each { |frame| puts "     #{frame}" }
end

sampler.stop
~~~

## Best Practices

### When Tracking in Production

1. **Start tracking AFTER startup**: Call `GC.start` before `capture.start` to avoid counting initialization objects
2. **Use count-only mode for monitoring**: `capture.track(Hash)` (no callback) has minimal overhead
3. **Enable analysis only when investigating**: Call path analysis has higher overhead
4. **Sample periodically**: Take measurements every 60 seconds rather than continuously
5. **Stop when done**: Always call `stop()` to remove event hooks

### Performance Considerations

**Count-only tracking** (no callback):
- Minimal overhead (~5-10% on allocation hotpath)
- Safe for production monitoring
- Tracks all classes automatically

**Call path analysis** (with callback):
- Higher overhead (captures `caller_locations` on every allocation)  
- Use during investigation, not continuous monitoring
- Only track specific classes you're investigating

### Avoiding False Positives

Objects allocated before tracking starts but freed after will show as negative or zero:

~~~ ruby
# ❌ Wrong - counts existing objects:
capture.start
100.times { {} }
GC.start  # Frees old + new objects → underflow

# ✅ Right - clean slate first:
GC.start  # Clear old objects
capture.start
100.times { {} }
~~~

## Common Scenarios

### Detecting Cache Leaks

~~~ ruby
# Monitor your cache class:
capture = Memory::Tracker::Capture.new
capture.start

cache_baseline = capture.count_for(CacheEntry)

# Run for a period:
sleep 300  # 5 minutes

cache_current = capture.count_for(CacheEntry)

if cache_current > cache_baseline * 2
  puts "⚠️  Cache is leaking! #{cache_current - cache_baseline} entries added"
  # Enable detailed tracking to find the source
end
~~~

### Finding Retention in Request Processing

~~~ ruby
# Track during request processing:
sampler = Memory::Tracker::Sampler.new
sampler.track_with_analysis(Hash)
sampler.start

# Process requests:
1000.times do
  process_request
end

# Check if Hashes are being retained:
statistics = sampler.statistics(Hash)

if statistics[:live_count] > 1000
  puts "Leaking #{statistics[:live_count]} Hashes per 1000 requests!"
  statistics[:top_paths].first(3).each do |path_data|
    puts "\n#{path_data[:count]}x from:"
    puts path_data[:path].join("\n  ")
  end
end

sampler.stop
~~~
