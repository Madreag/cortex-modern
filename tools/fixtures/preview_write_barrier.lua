local path = "UserScenes.rte/preview_write_barrier.lua"

function Create(self)
    PreviewBarrierFixture = PreviewBarrierFixture or {nested = {values = {count = 0, list = {}}}}
end

function Update(self)
    if IsAHuman(self) then
        local item = self.EquippedItem
        if item and not item:HasScript(path) then item:AddScript(path) end
    end
end

function OnFire(self)
    local values = PreviewBarrierFixture.nested.values
    local alias = values.list
    local prior = values.count
    values.count = prior + 1
    rawset(values, "last", self.UniqueID)
    table.insert(alias, values.count)
    assert(rawequal(alias, values.list) and #alias == values.count)
    assert(rawget(values, "count") == prior + 1 and getmetatable(values) == nil)
    print("[preview-barrier-fixture] count=" .. values.count .. " list=" .. #alias .. " uid=" .. self.UniqueID)
end
