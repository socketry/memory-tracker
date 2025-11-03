# Rack Integration

This guide explains how to integrate `memory-profiler` into Rack applications for automatic memory leak detection.

## Overview

The Rack middleware pattern provides a clean way to add memory monitoring to your application. The sampler runs in a background thread, automatically detecting leaks without impacting request processing.

## Basic Middleware

Create a middleware that monitors memory in the background:

~~~ ruby
# app/middleware/memory_monitoring.rb
require 'console'
require 'memory/profiler'

class MemoryMonitoring
	def initialize(app)
		@app = app
		
		# Create sampler with automatic leak detection:
		@sampler = Memory::Profiler::Sampler.new(
			# Use up to 10 caller locations for leak call graph analysis:
			depth: 10,
			# Enable detailed tracking after 10 increases:
			increases_threshold: 10
		)
		
		@sampler.start
		Console.info("Memory monitoring enabled")
		
		# Background thread runs periodic sampling:
		@thread = Thread.new do
			@sampler.run(interval: 60) do |sample|
				Console.warn(sample.target, "Memory usage increased!", sample: sample)
				
				# After threshold, get leak sources:
				if sample.increases >= 10
					if statistics = @sampler.statistics(sample.target)
						Console.error(sample.target, "Memory leak analysis:", statistics: statistics)
					end
				end
			end
		end
	end
	
	def call(env)
		@app.call(env)
	end
	
	def shutdown
		@thread&.kill
		@sampler&.stop!
	end
end
~~~

## Adding to config.ru

Add the middleware to your Rack application:

~~~ ruby
# config.ru
require_relative 'app/middleware/memory_monitoring'

use MemoryMonitoring

run YourApp
~~~
