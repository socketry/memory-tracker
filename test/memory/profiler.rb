# frozen_string_literal: true

# Released under the MIT License.
# Copyright, 2025, by Samuel Williams.

require "memory/profiler"

describe Memory::Profiler do
	with ".address_of" do
		it "returns memory address as hex string" do
			object = Object.new
			address = Memory::Profiler.address_of(object)
			
			expect(address).to be_a(String)
			expect(address).to be =~ /^0x[0-9a-f]+$/
		end
		
		it "returns same address for same object" do
			object = Object.new
			address1 = Memory::Profiler.address_of(object)
			address2 = Memory::Profiler.address_of(object)
			
			expect(address1).to be == address2
		end
		
		it "returns different addresses for different objects" do
			object1 = Object.new
			object2 = Object.new
			
			address1 = Memory::Profiler.address_of(object1)
			address2 = Memory::Profiler.address_of(object2)
			
			expect(address1).not.to be == address2
		end
		
		it "works with various object types" do
			objects = [
				"string",
				42,
				3.14,
				:symbol,
				[1, 2, 3],
				{key: "value"},
				Object.new,
				Class.new,
			]
			
			objects.each do |object|
				address = Memory::Profiler.address_of(object)
				expect(address).to be_a(String)
				expect(address).to be =~ /^0x[0-9a-f]+$/
			end
		end
		
		it "works with nil" do
			address = Memory::Profiler.address_of(nil)
			expect(address).to be_a(String)
			expect(address).to be =~ /^0x[0-9a-f]+$/
		end
		
		it "works with true and false" do
			true_address = Memory::Profiler.address_of(true)
			false_address = Memory::Profiler.address_of(false)
			
			expect(true_address).to be_a(String)
			expect(false_address).to be_a(String)
			expect(true_address).not.to be == false_address
		end
	end
end

