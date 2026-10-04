-- An unchanged mod keeps a long chain in its instance: a preview copies it whole, and a chain too deep for the copy to walk
-- is refused by name, never overflowed.
local function Chain(length)
	local head = {index = 0};
	local node = head;
	for index = 1, length do
		node.next = {index = index};
		node = node.next;
	end
	return head;
end

function Create(self)
	self.chain = Chain(1500);
	self.makeChain = Chain;
end

-- The hook the self-test calls so the script's Create runs.
function OnMessage(self, message)
end
