-- Cross-arch LuaJIT transcendental divergence probe (round 16).
-- Dumps op,index,arg-bits,result-bits (raw 8-byte LE hex of the double) for a
-- dense sweep of each op over its domain. Align the cross-platform diff by
-- (op,index): both platforms use identical integer index -> identical intended
-- arg, so any result-bit difference is a genuine libm/op divergence.
local ffi = require("ffi")
local buf = ffi.new("double[1]")
local function bits(x)
	buf[0] = x
	local p = ffi.cast("uint8_t*", buf)
	return string.format("%02x%02x%02x%02x%02x%02x%02x%02x",
		p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7])
end

-- name, fn, domain lo, hi  (domains kept strictly valid -> no NaN, JIT-friendly)
local ops = {
	{ "sin",      function(x) return math.sin(x) end,  -12.5, 12.5 },
	{ "cos",      function(x) return math.cos(x) end,  -12.5, 12.5 },
	{ "tan",      function(x) return math.tan(x) end,   -1.5,  1.5 },
	{ "asin",     function(x) return math.asin(x) end,  -1.0,  1.0 },
	{ "acos",     function(x) return math.acos(x) end,  -1.0,  1.0 },
	{ "atan",     function(x) return math.atan(x) end, -12.5, 12.5 },
	{ "tanh",     function(x) return math.tanh(x) end,  -8.0,  8.0 },
	{ "exp",      function(x) return math.exp(x) end,   -8.0,  8.0 },
	{ "log",      function(x) return math.log(x) end,  1e-6,  50.0 },
	{ "sqrt",     function(x) return math.sqrt(x) end,   0.0, 100.0 },
	{ "powfn2.5", function(x) return math.pow(x, 2.5) end, 0.0, 10.0 },
	{ "pow_op25", function(x) return x ^ 2.5 end,         0.0, 10.0 }, -- ^ non-integer
	{ "pow_op3",  function(x) return x ^ 3 end,         -10.0, 10.0 }, -- ^ integer exponent
}

local N = 2000
local out = assert(io.open(arg[1] or "luaprobe.txt", "w"))
for _, op in ipairs(ops) do
	local name, f, lo, hi = op[1], op[2], op[3], op[4]
	local step = (hi - lo) / N
	for i = 0, N do
		local x = lo + step * i
		local y = f(x)
		if y == y then -- skip any NaN defensively
			out:write(name, ",", tostring(i), ",", bits(x), ",", bits(y), "\n")
		end
	end
end
out:close()
io.write("luaprobe done: ", arg[1] or "luaprobe.txt", "\n")
