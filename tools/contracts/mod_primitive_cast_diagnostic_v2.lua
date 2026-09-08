-- Diagnostic only: isolate ownership/casting before any checkpoint assertion.
function Create(self) self.testCreate, self.testUpdate = 1, 0 end
function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    if self.UniqueID ~= 1048577 then return end
    local state = {source=CreateAHuman("Green Dummy", "Base.rte")}
    state.source:SetSpritePixelIndex(1, 1, 0, 19, -1, false)
    local p = Vector(81.25, 107.5)
    local values = {
        LinePrimitive(0, p, Vector(143, 157), 3.75, 47),
        ArcPrimitive(1, p, 0.375, 2.25, 29, 3, 51),
        SplinePrimitive(2, p, Vector(18, 21), Vector(32, 46), Vector(151, 173), 57),
        BoxPrimitive(3, p, Vector(153, 179), 61),
        BoxFillPrimitive(0, p, Vector(159, 181), 67),
        RoundedBoxPrimitive(1, p, Vector(161, 191), 5, 71),
        RoundedBoxFillPrimitive(2, p, Vector(163, 193), 9, 73),
        CirclePrimitive(3, p, 33, 79), CircleFillPrimitive(0, p, 37, 83),
        EllipsePrimitive(1, p, 18, 29, 89), EllipseFillPrimitive(2, p, 21, 31, 97),
        TrianglePrimitive(3, p, Vector(131, 112), Vector(111, 142), 101),
        TriangleFillPrimitive(0, p, Vector(132, 113), Vector(112, 143), 103),
        TextPrimitive(1, p, "completed tick 42", true, 1, 0.375),
        BitmapPrimitive(2, p, state.source, 0.25, 0, 1.75, true, false),
        BitmapPrimitive(3, p, "Base.rte/GUIs/Skins/Cursor.png", -0.25, false, true),
    }
    local expired = CreateAHuman("Green Dummy", "Base.rte")
    expired:SetSpritePixelIndex(1, 1, 0, 95, -1, false)
    values[#values + 1] = BitmapPrimitive(0, Vector(201, 219), expired, 0.5, 0, false, true)
    expired = nil
    collectgarbage("collect")
    values[1].contractLabel, values[1].contractRoot = "retained line", state
    for index, primitive in ipairs(values) do
        local descriptor = {_ScriptGraphNative(primitive)}
        local ok, why = pcall(function() PrimitiveMan:DrawPrimitives(73, {primitive}) end)
        if not self.primitiveDiagnosticPrinted then
            print("[primitive-cast-diagnostic] tick=" .. self.testUpdate .. " index=" .. index .. " native=" .. tostring(descriptor[1]) .. " class=" .. tostring(descriptor[2]) .. " result=" .. tostring(ok) .. " detail=" .. tostring(why))
        end
    end
    local vertex = Vector(19.25, -31.5)
    local polygonOK, polygonWhy = pcall(function() PrimitiveMan:DrawPolygonPrimitive(2, Vector(211, 229), 107, {vertex, Vector(5, 7), vertex}) end)
    if not self.primitiveDiagnosticPrinted then print("[primitive-vertex-diagnostic] result=" .. tostring(polygonOK) .. " detail=" .. tostring(polygonWhy)) end
    self.primitiveDiagnosticPrinted = true
end