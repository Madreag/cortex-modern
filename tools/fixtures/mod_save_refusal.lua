-- Holds a world object in Update's upvalue chain so a save must refuse it once it dies.
local transactionOwner = CreateMOPixel("Spark Yellow 1", "Base.rte");
local fn = function()
	return transactionOwner;
end
local function Update()
	return fn();
end

return { Update = Update, UniqueID = transactionOwner.UniqueID };
