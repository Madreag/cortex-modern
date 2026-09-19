-- Same body the script-graph selftest runs on window-armed _PreviewDepth / _PreviewSlack.
local function keys(t)
	local seq = {}
	for k in pairs(t) do
		seq[#seq + 1] = tostring(k)
	end
	table.sort(seq)
	return table.concat(seq, ',')
end

local function ensure()
	if type(_PreviewDepth) ~= 'table' then
		_PreviewDepth = { d1 = { v = 1 }, d3 = { a = { b = { v = 1, gone = 1 } } }, d8 = { a = { b = { c = { d = { e = { f = { g = { v = 1, gone = 1 } } } } } } } } }
	end
	if type(_PreviewSlack) ~= 'table' then
		_PreviewSlack = {}
		for i = 1, 5 do
			_PreviewSlack[i] = i
		end
		_PreviewSlack[7] = 7
		_PreviewSlack[7] = nil
	end
end

local function probe()
	ensure()
	if _PreviewModCompatSnap == nil then
		_PreviewModCompatSnap = {
			depth_keys = keys(_PreviewDepth),
			slack_len = #_PreviewSlack,
			depth_meta = getmetatable(_PreviewDepth),
			slack_meta = getmetatable(_PreviewSlack),
		}
	end
	local snap = _PreviewModCompatSnap
	assert(keys(_PreviewDepth) == snap.depth_keys, 'pairs content changed under a window')
	assert(#_PreviewSlack == snap.slack_len, 'length operator changed under a window')
	assert(getmetatable(_PreviewDepth) == snap.depth_meta, 'getmetatable identity changed under a window')
	assert(getmetatable(_PreviewSlack) == snap.slack_meta, 'getmetatable identity changed under a window')
	local prior = rawget(_PreviewSlack, 1)
	assert(rawset(_PreviewSlack, 1, prior) == _PreviewSlack and rawget(_PreviewSlack, 1) == prior, 'rawset return or slot changed under a window')
	print('[preview-modcompat-fixture] PASS pairs length rawset getmetatable')
end

probe()

function Create(self)
	probe()
end
