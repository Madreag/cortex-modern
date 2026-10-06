function VWSceneObserver:StartScript()
	self.ticks = 0;
	self.playTicks = 0;
	self.lastScene = "";
	self.lastForm = "";
	MetricsCollector:BeginRun("Void Wanderers scene", 42);
end

function VWSceneObserver:UpdateScript()
	self.ticks = self.ticks + 1;
	local scene = SceneMan.Scene and SceneMan.Scene.PresetName or "none";
	local mode = CF and CF.GS and CF.GS["Mode"] or "none";
	local form = VoidWanderers and VoidWanderers.UI and VoidWanderers.UI[1] and VoidWanderers.UI[1].Text or "none";
	if self.ticks <= 80 and VoidWanderers and VoidWanderers.Mouse then
		local cursor = VoidWanderers.PlayerCount == 1 and VoidWanderers.CurCursorMO or VoidWanderers.brain;
		if cursor and MovableMan:IsActor(cursor) then
			print("[vw-hand] tick=" .. self.ticks .. " x=" .. VoidWanderers.Mouse.X .. " y=" .. VoidWanderers.Mouse.Y .. " fire=" .. tostring(cursor:GetController():IsState(Controller.WEAPON_FIRE)) .. " pressed=" .. tostring(VoidWanderers.MouseFirePressed) .. " form=" .. form);
		end
	end
	if form ~= self.lastForm then
		print("[vw-form] tick=" .. self.ticks .. " form=" .. form);
		self.lastForm = form;
		if VoidWanderers.Mouse and VoidWanderers.Mid then
			print("[vw-layout] mouse_x=" .. VoidWanderers.Mouse.X .. " mouse_y=" .. VoidWanderers.Mouse.Y .. " mid_x=" .. VoidWanderers.Mid.X .. " mid_y=" .. VoidWanderers.Mid.Y .. " width=" .. FrameMan.PlayerScreenWidth .. " height=" .. FrameMan.PlayerScreenHeight);
		end
		if VoidWanderers.FactionButtons then
			for index, button in ipairs(VoidWanderers.FactionButtons) do
				print("[vw-faction] index=" .. index .. " name=" .. button.FactionName .. " x=" .. button.Pos.X .. " y=" .. button.Pos.Y);
			end
		end
	end
	if scene == "Vessel Lynx" and mode == "Vessel" then
		self.playTicks = self.playTicks + 1;
		if self.playTicks == 1 then
			print("[vw-ship] first_tick=" .. self.ticks .. " scene=" .. scene .. " mode=" .. mode);
		end
	end
	if scene ~= self.lastScene or self.ticks % 60 == 0 then
		print("[vw-scene] tick=" .. self.ticks .. " scene=" .. scene .. " mode=" .. mode .. " play_ticks=" .. self.playTicks);
		self.lastScene = scene;
		local activity = ActivityMan:GetActivity();
		for player = 0, 3 do
			if activity:PlayerActive(player) and activity:PlayerHuman(player) then
				local actor = ToGameActivity(activity):GetControlledActor(player);
				if actor and MovableMan:IsActor(actor) then
					local detached = CF and CF.GS and CF.GS["Brain" .. player .. "Detached"] or "False";
					local controller = actor:GetController();
					print("[vw-seat] tick=" .. self.ticks .. " player=" .. player .. " actor=" .. actor.PresetName .. " x=" .. actor.Pos.X .. " y=" .. actor.Pos.Y .. " uid=" .. actor.UniqueID .. " detached=" .. detached .. " brain_player=" .. actor:GetNumberValue("VW_BrainOfPlayer") .. " left=" .. tostring(controller:IsState(Controller.MOVE_LEFT)) .. " right=" .. tostring(controller:IsState(Controller.MOVE_RIGHT)));
				end
			end
		end
		local activity = ToGameActivity(ActivityMan:GetActivity());
		for player = 0, 3 do
			if activity:PlayerActive(player) and activity:PlayerHuman(player) then
				local brain = activity:GetPlayerBrain(player);
				local banner = activity:GetBanner(0, player);
				if brain and MovableMan:IsActor(brain) and not brain:IsDead() and banner:IsVisible() and banner.BannerText == "DEAD" then
					print("[vw-visible] live_brain_under_dead_banner player=" .. player);
				end
			end
		end
	end
	if self.playTicks >= 1800 and self.ticks % 60 == 0 then
		self:WriteReport();
	end
end

function VWSceneObserver:WriteReport()
	MetricsCollector:Record("vw_scene_play_ticks", self.playTicks);
	MetricsCollector:SetResult(self.playTicks >= 1800);
	MetricsCollector:WriteReport("Userdata/VWSceneTrace.json");
end

function VWSceneObserver:EndScript()
	self:WriteReport();
	MetricsCollector:EndRun();
end
