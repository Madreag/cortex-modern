-- An unchanged mod's script graph: functions used as keys, variables shared between closures and suspended coroutines,
-- closures over self, a counter only the instance reaches, an aliased Vector and a part handle used as a key.
-- OnMessage reports what each case reads; a preview's copy must read exactly what the live instance reads.

-- f and a suspended coroutine's body share a variable that is closed.
local function ClosedPair()
	local shared = 0;
	local f = function()
		shared = shared + 1;
		return shared;
	end
	local co = coroutine.create(function()
		while true do
			shared = shared + 10;
			coroutine.yield(shared);
		end
	end);
	coroutine.resume(co);
	return f, co;
end

-- f shares a variable still open on a suspended coroutine's stack.
local function OpenPair()
	local co = coroutine.create(function()
		local shared = 0;
		local f = function()
			shared = shared + 1;
			return shared;
		end
		coroutine.yield(f);
		while true do
			shared = shared + 10;
			coroutine.yield(shared);
		end
	end);
	local _, f = coroutine.resume(co);
	return f, co;
end

-- The function comes before the coroutine, or the coroutine before both the function and the table keyed by it.
local function Arrange(f, co, functionFirst)
	local keyed = {[f] = "kept"};
	if functionFirst then
		return {f, keyed, co, functionFirst = true};
	end
	return {co, keyed, f, functionFirst = false};
end

local function ProbePair(graph)
	local f, keyed, co;
	if graph.functionFirst then
		f, keyed, co = graph[1], graph[2], graph[3];
	else
		co, keyed, f = graph[1], graph[2], graph[3];
	end
	local _, start = debug.getupvalue(f, 1);
	local key = rawget(keyed, f);
	local first = f();
	local _, resumed = coroutine.resume(co);
	local second = f();
	return "start=" .. tostring(start) .. " key=" .. tostring(key) .. " f=" .. tostring(first) .. " co=" .. tostring(resumed) .. " f=" .. tostring(second);
end

-- f, held only by a table, holds a variable open on one coroutine and a closed one another coroutine's body shares.
local function Mixed()
	local outer = 0;
	local holder = {};
	local body = coroutine.create(function()
		while true do
			outer = outer + 100;
			coroutine.yield(outer);
		end
	end);
	local opener = coroutine.create(function()
		local inner = 0;
		local function make()
			return function()
				inner = inner + 1;
				outer = outer + 1;
				return inner .. "/" .. outer;
			end
		end
		holder.f = make();
		coroutine.yield();
		while true do
			inner = inner + 10;
			coroutine.yield(inner);
		end
	end);
	coroutine.resume(opener);
	coroutine.resume(body);
	return {holder, body, opener};
end

local function ProbeMixed(graph)
	local f, body, opener = graph[1].f, graph[2], graph[3];
	local first = f();
	local _, outer = coroutine.resume(body);
	local _, inner = coroutine.resume(opener);
	local second = f();
	return "f=" .. tostring(first) .. " body=" .. tostring(outer) .. " opener=" .. tostring(inner) .. " f=" .. tostring(second);
end

-- A function with no variables sits in a table and on a suspended coroutine's stack.
local function Plain()
	local plain = function(x) return x; end
	local list = {plain};
	local co = coroutine.create(function(fn, held)
		while true do
			coroutine.yield(fn == held[1]);
		end
	end);
	coroutine.resume(co, plain, list);
	return {list, co};
end

local function ProbePlain(graph)
	local _, same = coroutine.resume(graph[2]);
	return "same=" .. tostring(same);
end

function Create(self)
	local closedFunction, closedCoroutine = ClosedPair();
	local closedFunction2, closedCoroutine2 = ClosedPair();
	local openFunction, openCoroutine = OpenPair();
	local openFunction2, openCoroutine2 = OpenPair();
	self.identityCases = {
		closed_function_first = Arrange(closedFunction, closedCoroutine, true),
		closed_coroutine_first = Arrange(closedFunction2, closedCoroutine2, false),
		open_function_first = Arrange(openFunction, openCoroutine, true),
		open_coroutine_first = Arrange(openFunction2, openCoroutine2, false),
	};
	self.mixedCase = Mixed();
	self.plainCase = Plain();

	self.me = function()
		return self;
	end
	self.mark = function(value)
		self.marked = value;
	end

	local count = 0;
	self.count = function()
		count = count + 1;
		return count;
	end

	local position = Vector(1, 2);
	self.vectorA = position;
	self.vectorB = position;

	local part = self.Head;
	self.part = part;
	self.parts = part and {[part] = "kept"} or {};

	local wrapped = coroutine.wrap(function()
		while true do
			coroutine.yield(1);
		end
	end);
	self.wrapCase = {wrapped, {[wrapped] = "kept"}};

	local meta = {};
	self.metaCase = {setmetatable({}, meta), meta};
end

local c_PairNames = {"closed_function_first", "closed_coroutine_first", "open_function_first", "open_coroutine_first"};

function OnMessage(self, message)
	if message ~= "live" and message ~= "preview" then
		return;
	end
	local lines = {};
	local function report(name, probe)
		local ok, result = pcall(probe);
		lines[#lines + 1] = name .. " " .. (ok and tostring(result) or ("error " .. tostring(result)));
	end
	for _, name in ipairs(c_PairNames) do
		report(name, function() return ProbePair(self.identityCases[name]); end);
	end
	report("mixed_open_and_closed", function() return ProbeMixed(self.mixedCase); end);
	report("plain_function_on_a_coroutine", function() return ProbePlain(self.plainCase); end);
	report("closure_returns_self", function() return "same=" .. tostring(rawequal(self.me(), self)); end);
	report("closure_writes_through_self", function()
		local before = self.marked;
		self.mark(7);
		local after = self.marked;
		self.marked = nil;
		return "before=" .. tostring(before) .. " after=" .. tostring(after);
	end);
	report("instance_counter", function()
		local _, start = debug.getupvalue(self.count, 1);
		return "start=" .. tostring(start) .. " next=" .. tostring(self.count());
	end);
	report("vector_alias", function()
		local same = rawequal(self.vectorA, self.vectorB);
		self.vectorA.X = self.vectorA.X + 1;
		return "same=" .. tostring(same) .. " x=" .. tostring(self.vectorB.X);
	end);
	report("part_handle_key", function()
		if not self.part then
			return "no part";
		end
		return "key=" .. tostring(self.parts[self.part]);
	end);
	report("wrap_key", function() return "key=" .. tostring(rawget(self.wrapCase[2], self.wrapCase[1])); end);
	report("metatable_identity", function() return "same=" .. tostring(rawequal(getmetatable(self.metaCase[1]), self.metaCase[2])); end);
	_ScriptFieldsStash = _ScriptFieldsStash or {};
	_ScriptFieldsStash["preview-identity:" .. message] = table.concat(lines, "\n");
end
