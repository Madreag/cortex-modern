function Create(self)
	local cases = {
		function() MovableMan:AddActor("invalid actor") end,
		function() CreateAHuman({}) end,
		function() return Vector("invalid coordinate") end,
		function() local vector = Vector(); vector.X = {} end
	};
	local checked = 0;
	for repetition = 1, 100 do
		for _, invalidCall in ipairs(cases) do
			local ok, message = pcall(invalidCall);
			assert(not ok and type(message) == "string", "invalid native argument did not raise a Lua error");
			checked = checked + 1;
		end
	end
	local task = coroutine.create(function()
		local ok, message = pcall(cases[1]);
		assert(not ok and message:find("AddActor", 1, true), "coroutine lost the native error");
	end);
	assert(coroutine.resume(task));
	assert(coroutine.status(task) == "dead");
	collectgarbage("collect");
	assert(Vector(3, 4).Magnitude == 5, "valid native calls stopped working after an error");
	print("[lua-binding-errors] PASS uid=" .. self.UniqueID .. " cases=" .. (checked + 1));
end
