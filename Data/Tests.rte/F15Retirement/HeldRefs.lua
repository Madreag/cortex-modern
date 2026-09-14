-- Holds ordinary Lua references to a canonical object across a preview retirement boundary.

F15Refs = {}

local failures = {}

local function report(name, ok, detail)
	print(string.format("[f15refs] %s %s: %s", ok and "PASS" or "FAIL", name, detail))
	if not ok then
		failures[#failures + 1] = name .. ": " .. detail
	end
end

function F15Refs.Capture(device)
	failures = {}
	F15Refs.uid = device.UniqueID
	F15Refs.name = device.PresetName
	F15Refs.closure = function() return device end
	F15Refs.first = {device}
	F15Refs.second = {held = device}
	F15Refs.continuation = coroutine.create(function()
		local held = device
		coroutine.yield("suspended")
		return held.UniqueID, held.PresetName
	end)
	local resumed, state = coroutine.resume(F15Refs.continuation)
	F15Refs.alias = device.Pos
	F15Refs.originalX = device.Pos.X
	report("capture_before_preview", resumed and state == "suspended" and F15Refs.alias ~= nil and rawequal(F15Refs.first[1], F15Refs.second.held),
		string.format("uid=%s name=%s continuation=%s x=%s", tostring(F15Refs.uid), tostring(F15Refs.name), tostring(state), tostring(F15Refs.originalX)))
	if #failures > 0 then
		error("f15refs capture: " .. table.concat(failures, "; "), 0)
	end
end

function F15Refs.CheckLink(holder)
	local target = holder:GetWhichMOToNotHit()
	local detail = "nothing"
	if target then
		detail = string.format("points at %s uid=%s", tostring(target.PresetName), tostring(target.UniqueID))
	end
	report("held_link_after_retirement", target == nil, detail)
	if #failures > 0 then
		error("f15refs link: " .. table.concat(failures, "; "), 0)
	end
end

function F15Refs.Verify(device)
	local held = F15Refs.closure()
	report("closure_owner_after_preview", held ~= nil and held.UniqueID == F15Refs.uid and rawequal(held, F15Refs.first[1]),
		string.format("uid=%s expected=%s", tostring(held and held.UniqueID), tostring(F15Refs.uid)))
	report("shared_identity_after_preview", rawequal(F15Refs.first[1], F15Refs.second.held),
		string.format("observed=%s expected=true", tostring(rawequal(F15Refs.first[1], F15Refs.second.held))))
	local resumed, uid, name = coroutine.resume(F15Refs.continuation)
	report("continuation_after_preview", resumed and uid == F15Refs.uid and name == F15Refs.name and coroutine.status(F15Refs.continuation) == "dead",
		string.format("resumed=%s uid=%s name=%s status=%s", tostring(resumed), tostring(uid), tostring(name), coroutine.status(F15Refs.continuation)))
	F15Refs.alias.X = 17
	report("property_alias_write_after_preview", device.Pos.X == 17, string.format("owner.X=%s expected=17", tostring(device.Pos.X)))
	F15Refs.alias.X = F15Refs.originalX
	report("property_alias_restored", device.Pos.X == F15Refs.originalX, string.format("owner.X=%s expected=%s", tostring(device.Pos.X), tostring(F15Refs.originalX)))
	collectgarbage("collect")
	local afterGC = F15Refs.closure()
	report("closure_after_collect", afterGC ~= nil and afterGC.UniqueID == F15Refs.uid and rawequal(afterGC, held),
		string.format("uid=%s expected=%s", tostring(afterGC and afterGC.UniqueID), tostring(F15Refs.uid)))
	if #failures > 0 then
		error("f15refs verify: " .. table.concat(failures, "; "), 0)
	end
end

function F15Refs.Release()
	F15Refs = nil
	collectgarbage("collect")
end
