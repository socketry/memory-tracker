# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler/graph"

class LeakyObject
	def initialize
		@children = []
	end
	
	def <<(object)
		@children << object
	end
	
	attr_reader :children
end

class CircularObject
	attr_accessor :next, :data
	
	def initialize(data)
		@data = data
		@next = nil
	end
end

describe Memory::Profiler::Graph do
	let(:graph) {subject.new}
	
	with "#add" do
		it "can add an object" do
			root = LeakyObject.new
			100.times do |i|
				hash = Hash.new
				root << hash
				graph.add(hash)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first).to have_keys(
				name: be(:end_with?, "LeakyObject@children"),
				count: be == 100,
				percentage: be == 100.0,
			)
		end
	end
	
	with "circular references" do
		it "handles simple circular reference" do
			# Create A -> B -> A cycle
			obj_a = CircularObject.new("A")
			obj_b = CircularObject.new("B")
			obj_a.next = obj_b
			obj_b.next = obj_a
			
			graph.add(obj_a)
			graph.add(obj_b)
			
			# Should complete without infinite loop
			graph.update!(obj_a)
			
			expect(graph.roots.size).to be >= 1
		end
		
		it "handles self-referential object" do
			obj = CircularObject.new("self")
			obj.next = obj  # Points to itself
			
			graph.add(obj)
			graph.update!(obj)
			
			# Should not crash
			expect(graph.roots).to be_a(Array)
		end
		
		it "handles complex circular graph" do
			# Create a diamond with circular references
			root = LeakyObject.new
			
			objs = 10.times.map{|i| CircularObject.new(i)}
			
			# Create circular chain
			objs.each_with_index do |obj, i|
				obj.next = objs[(i + 1) % objs.size]
				root << obj
				graph.add(obj)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first).to have_keys(
				count: be == 10,
			)
		end
	end
	
	with "nested structures" do
		it "handles deeply nested arrays" do
			root = []
			current = root
			
			10.times do
				inner = []
				current << inner
				graph.add(inner)
				current = inner
			end
			
			graph.update!(root)
			roots = graph.roots
			
			# Root array should be the top retainer
			expect(roots.first[:count]).to be == 10
		end
		
		it "handles hash with many keys" do
			root = {}
			
			100.times do |i|
				value = {data: i}
				root["key_#{i}"] = value
				graph.add(value)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:count]).to be == 100
		end
		
		it "handles mixed hash and array structures" do
			root = {arrays: [], hashes: {}}
			
			50.times do |i|
				arr = [i]
				root[:arrays] << arr
				graph.add(arr)
			end
			
			50.times do |i|
				h = {value: i}
				root[:hashes]["key_#{i}"] = h
				graph.add(h)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			# Should find both retaining paths
			expect(roots.first[:count]).to be == 100
		end
	end
	
	with "multiple roots" do
		it "identifies multiple independent retainers" do
			root1 = LeakyObject.new
			root2 = LeakyObject.new
			
			50.times do
				obj = {owner: :root1}
				root1 << obj
				graph.add(obj)
			end
			
			30.times do
				obj = {owner: :root2}
				root2 << obj
				graph.add(obj)
			end
			
			# Update from a common container
			container = {r1: root1, r2: root2}
			graph.update!(container)
			
			roots = graph.roots
			
			# Should have entries for both root's children arrays
			expect(roots.size).to be >= 2
			expect(roots[0][:count] + roots[1][:count]).to be >= 80
		end
	end
	
	with "name generation" do
		it "generates names for constants" do
			module TestModule
				CONSTANT = {data: "test"}
			end
			
			graph.add(TestModule::CONSTANT[:data])
			graph.update!(TestModule)
			
			roots = graph.roots
			expect(roots.first[:name]).to be =~ /CONSTANT/
		end
		
		it "generates names for instance variables" do
			root = LeakyObject.new
			obj = {test: true}
			root << obj
			graph.add(obj)
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:name]).to be =~ /@children/
		end
	end
	
	with "edge cases" do
		it "handles empty object set" do
			graph.update!(Object)
			roots = graph.roots
			
			expect(roots).to be == []
		end
		
		it "handles object with no parents" do
			orphan = {orphan: true}
			graph.add(orphan)
			
			graph.update!(orphan)
			roots = graph.roots
			
			# Orphan should not have a parent
			expect(roots).to be == []
		end
		
		it "handles very large object counts" do
			root = LeakyObject.new
			
			1000.times do |i|
				obj = {index: i}
				root << obj
				graph.add(obj)
			end
			
			graph.update!(root)
			roots = graph.roots
			
			expect(roots.first[:count]).to be == 1000
		end
	end
end
