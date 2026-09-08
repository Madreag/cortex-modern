-- Public drawing calls populate the real completed-tick queue. The independent
-- native audit reads every primitive, bitmap, sprite link and vertex directly.
function Create(self)
    self.testCreate, self.testUpdate = 1, 0
end

function Update(self)
    self.testUpdate = self.testUpdate + 1
    self:SetNumberValue("TestUpdates", self.testUpdate)
    if self.UniqueID ~= 1048577 then return end
    local state = self.primitiveContracts
    if not state then
        local source = CreateAHuman("Green Dummy", "Base.rte")
        assert(source:SetSpritePixelIndex(1, 1, 0, 19, -1, false))
        state = {source=source, sourceAlias=source}
        self.primitiveContracts = state
        local root = self
        _G._ContractAuditPerturb = function()
            local saved = root.primitiveContracts
            saved.vertex.X, saved.vertex.Y = -701.25, 809.5
            saved.source:SetSpritePixelIndex(1, 1, 0, 73, -1, false)
            saved.primitives[1].contractLabel = "perturbed"
            PrimitiveMan:DrawCircleFillPrimitive(0, Vector(901, 777), 31, 82)
            print("[primitive-contract-perturb] vertex pixels instance queue changed")
        end
        _G._ContractAuditCheck = function(stage)
            local saved = root.primitiveContracts
            local checked, failures = 0, 0
            local function check(name, valid)
                checked = checked + 1
                if not valid then failures = failures + 1; print("[reference-contract-mismatch] " .. stage .. " primitive." .. name) end
            end
            check("source_alias", rawequal(saved.source, saved.sourceAlias))
            check("source_pixel", saved.source:GetSpritePixelIndex(1, 1, 0) == 19)
            check("primitive_alias", rawequal(saved.primitives[1], saved.lineAlias))
            check("primitive_lua_instance", saved.primitives[1].contractLabel == "retained line" and saved.primitives[1].contractRoot == saved)
            check("vertex_alias", rawequal(saved.vertex, saved.vertexAlias))
            check("vertex_value", saved.vertex.X == 19.25 and saved.vertex.Y == -31.5)
            local vertex = _ScriptGraphOwnerReference(PrimitiveMan, "primitive-vertex", saved.vertexIndex, false)
            check("vertex_native_owner", vertex and _ScriptGraphNativeAddress(vertex) == _ScriptGraphNativeAddress(saved.vertex))
            for index, primitive in ipairs(saved.primitives) do
                local queued = _ScriptGraphOwnerReference(PrimitiveMan, "primitive", saved.indices[index], false)
                check("queued_owner_" .. index, queued and _ScriptGraphNativeAddress(primitive) == _ScriptGraphNativeAddress(queued))
            end
            check("owned_text_alias", rawequal(saved.ownedText, saved.ownedTextAlias))
            check("owned_text_lua_instance", saved.ownedText.contractLabel == "owned text")
            print("[reference-contract-check] " .. stage .. " checked=" .. checked .. " mismatches=" .. failures)
        end
    end
    -- FrameMan clears the previous tick's primitives before these drawing calls.
    -- Keep the newest native references so the capture must restore actual owners.
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
    PrimitiveMan:DrawPrimitives(73, values)
    local vertex = Vector(19.25, -31.5)
    PrimitiveMan:DrawPolygonPrimitive(2, Vector(211, 229), 107, {vertex, Vector(5, 7), vertex})
    PrimitiveMan:DrawPolygonFillPrimitive(3, Vector(223, 233), 109, {vertex, Vector(11, 13), Vector(17, 19)})
    state.primitives, state.lineAlias = values, values[1]
    state.vertex, state.vertexAlias = vertex, vertex
    local descriptor = {_ScriptGraphNative(vertex)}
    assert(descriptor[1] == "owner-ref", "polygon vertex has no native queue owner")
    state.vertexIndex = descriptor[4]
    state.indices = {}
    for index, primitive in ipairs(values) do
        local owner = {_ScriptGraphNative(primitive)}
        assert(owner[1] == "owner-ref", "scheduled primitive has no native queue owner")
        state.indices[index] = owner[4]
    end
    state.ownedText = TextPrimitive(0, Vector(301, 319), "unsubmitted text", false, 2, -0.125)
    state.ownedText.contractLabel = "owned text"
    state.ownedTextAlias = state.ownedText
end
