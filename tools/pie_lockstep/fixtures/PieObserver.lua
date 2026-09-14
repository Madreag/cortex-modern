local case = dofile("Userdata/UserScenes.rte/PieCase.lua");

function Create(self)
    self.pieObservationTick = 0;
    self.pieOpenCalls = 0;
    self.pieModeChanges = 0;
end

function WhilePieMenuOpen(self, pieMenu)
    self.pieOpenCalls = self.pieOpenCalls + 1;
end

function OnControllerInputModeChange(self, previousMode, previousPlayer)
    self.pieModeChanges = self.pieModeChanges + 1;
end

function Update(self)
    self.pieObservationTick = self.pieObservationTick + 1;
    local activity = ActivityMan:GetActivity();
    local controlled = activity:GetControlledActor(0);
    local localActor = controlled and controlled.UniqueID == self.UniqueID;
    if self.pieObservationTick == 130 and localActor and self.Team == Activity.TEAM_1 then
        if case == "actor_cancel" then
            ToGameActivity(activity):SetActorSelectCursor(self.CPUPos, 0);
            self:SetControllerMode(Controller.CIM_AI, -1);
            activity:SetViewState(Activity.ACTORSELECT, 0);
            print("[pie-fixture] enter ActorSelect uid=" .. self.UniqueID);
        elseif case == "delivery_cancel" then
            activity:SetViewState(Activity.LZSELECT, 0);
            print("[pie-fixture] enter LandingZoneSelect uid=" .. self.UniqueID);
        end
    end
    local pie = self.PieMenu;
    local values = {pie:IsEnabled(), pie:IsEnabling(), pie:IsDisabling(),
                    pie:IsEnablingOrDisabling(), pie:IsVisible(), pie:HasSubPieMenuOpen()};
    local state = "";
    for _, value in ipairs(values) do
        state = state .. (value and "1" or "0");
    end
    print("[pie-observe] uid=" .. self.UniqueID .. " step=" .. self.pieObservationTick
          .. " mode=" .. self:GetController().InputMode .. " state=" .. state
          .. " callbacks=" .. self.pieOpenCalls .. " modechanges=" .. self.pieModeChanges);
    if localActor then
        print("[pie-view] uid=" .. self.UniqueID .. " step=" .. self.pieObservationTick
              .. " view=" .. activity:GetViewState(0));
    end
end
