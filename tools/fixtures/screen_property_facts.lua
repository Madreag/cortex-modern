-- Reads the screen properties a mod can read, at whatever window the run was given. PlayerScreenWidth
-- and PlayerScreenHeight must keep answering this machine's window; the pinned pair must answer the
-- default window at every size. The reads are protected so the same fixture runs on a build that has
-- no pinned properties yet, which is what makes it a before-and-after check.
function ScreenPropertyFacts:StartActivity(isNewGame)
	local function read(name)
		local ok, value = pcall(function() return FrameMan[name]; end);
		if not ok or value == nil then
			return "absent";
		end
		return tostring(value);
	end
	print("[screenprops] player=" .. read("PlayerScreenWidth") .. "x" .. read("PlayerScreenHeight") ..
		" sim=" .. read("SimScreenWidth") .. "x" .. read("SimScreenHeight") ..
		" screens=" .. read("ScreenCount") .. " resmult=" .. read("ResolutionMultiplier"));
end

function ScreenPropertyFacts:UpdateActivity()
end
