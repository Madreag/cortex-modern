-- Reproduce the public predicates used by Gunsword.lua (RemoveWounds when
-- playback stops) and Nanolyzer.lua (conditional RNG before later sim work).
-- This fixture is separate from the read-only sound_observer.lua.
local mode = "__MODE__"

function Update(self)
    if self.UniqueID ~= 1048596 or not IsAHuman(self) then return end
    local actor = ToAHuman(self)
    local arm = actor.FGArm
    if not arm then return end
    if mode == "wounds" then
        local armed = self.soundQueryArmed or 0
        if armed < 4 and actor.Age >= 2000 + armed * 250 then
            local w = CreateAEmitter("Leaking Machinery", "Base.rte")
            assert(w)
            arm:AddWound(w, Vector(0, 0), false, false, true)
            self.soundQueryArmed = armed + 1
            print(string.format("[sound-query-armed] actor=%d age=%.6f wound=%d preset=%s",
                actor.UniqueID, actor.Age, w.UniqueID, w.PresetName))
        end
    end
    for wound in arm.Wounds do
        local sound = wound.BurstSound
        if sound and (mode == "rng_particles" or sound.PresetName == "Metal Impact Machinery") then
            local playing = sound:IsBeingPlayed()
            if mode == "wounds" then
                print(string.format("[sound-query-read] mode=wounds actor=%d arm=%d age=%.6f playing=%d wounds=%d",
                    actor.UniqueID, arm.UniqueID, actor.Age, playing and 1 or 0, arm.WoundCount))
                if not playing then
                    local before = arm.WoundCount
                    local woundId = wound.UniqueID
                    arm:RemoveWounds(1)
                    local after = arm.WoundCount
                    self.soundQueryRemoved = (self.soundQueryRemoved or 0) + before - after
                    print(string.format("[sound-query-effect] mode=wounds actor=%d wound=%d age=%.6f before=%d after=%d",
                        actor.UniqueID, woundId, actor.Age, before, after))
                end
            elseif mode == "rng" then
                local conditional = playing and RangeRand(-1, 1) or nil
                local following = RangeRand(-1, 1)
                self.soundQueryFollowingDraw = following
                print(string.format("[sound-query-effect] mode=rng actor=%d age=%.6f playing=%d conditional=%s following=%.17g",
                    actor.UniqueID, actor.Age, playing and 1 or 0, tostring(conditional), following))
            elseif mode == "rng_particles" then
                local conditional = playing and RangeRand(-1, 1) or nil
                local following = RangeRand(-1, 1)
                local piece = CreateMOPixel("Spark Yellow 1", "Base.rte")
                assert(piece)
                piece.Pos = arm.Pos
                piece.Vel = Vector(following * 3, -1)
                piece.Lifetime = 500 + math.floor((following + 1) * 250)
                MovableMan:AddParticle(piece)
                print(string.format("[sound-query-effect] mode=rng_particles actor=%d wound=%d age=%.6f playing=%d conditional=%s following=%.17g",
                    actor.UniqueID, wound.UniqueID, actor.Age, playing and 1 or 0, tostring(conditional), following))
            else
                error("unknown sound query gate mode")
            end
            if mode ~= "rng_particles" then break end
        end
    end
end
