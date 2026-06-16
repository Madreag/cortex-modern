-- Does LuaJIT's JIT honor a math.* table override on a hot (JIT-compiled) path?
-- Mirrors the engine: replace math.sin/tanh/asin (as the determinism override does),
-- then call them with a literal field access in a hot loop and count override hits.
local function run(label, fn, lo, hi)
	local s, n = 0.0, 0
	local N = 4000000
	for i = 0, N - 1 do
		local x = lo + (hi - lo) * (i % 1000) / 1000
		s = s + fn(x)
		n = n + 1
	end
	return s, n
end

-- sin
local realsin = math.sin
local sinhits = 0
math.sin = function(x) sinhits = sinhits + 1; return -7.0 end
local ssin, nsin = run("sin", function(x) return math.sin(x) end, 0, 1)
math.sin = realsin
print(string.format("sin : loop_iters=%d override_hits=%d honored=%s (sum=%.3g; if honored sum==-7*iters=%.3g)",
	nsin, sinhits, tostring(sinhits == nsin), ssin, -7.0 * nsin))

-- tanh (AI aim tolerance)
local realtanh = math.tanh
local tanhhits = 0
math.tanh = function(x) tanhhits = tanhhits + 1; return -7.0 end
local stanh, ntanh = run("tanh", function(x) return math.tanh(x) end, -1, 1)
math.tanh = realtanh
print(string.format("tanh: loop_iters=%d override_hits=%d honored=%s", ntanh, tanhhits, tostring(tanhhits == ntanh)))

-- asin (AI aim/dig angle)
local realasin = math.asin
local asinhits = 0
math.asin = function(x) asinhits = asinhits + 1; return -7.0 end
local sasin, nasin = run("asin", function(x) return math.asin(x) end, -1, 1)
math.asin = realasin
print(string.format("asin: loop_iters=%d override_hits=%d honored=%s", nasin, asinhits, tostring(asinhits == nasin)))

print("jit status:", (require("jit")).status())
