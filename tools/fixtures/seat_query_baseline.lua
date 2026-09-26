-- The FeelBaseline duel as a mod's activity that asks every seat's mouse and device on each update; the answers go into
-- the activity's own fields, which every peer holds alike.
dofile("Userdata/UserScenes.rte/FeelBaseline.lua");
local updateBaseline = FeelBaseline.UpdateActivity;

function FeelBaseline:UpdateActivity()
	updateBaseline(self);
	local digest = self.seatDigest or 0;
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		local movement = UInputMan:GetMouseMovement(player);
		local controller = self:GetPlayerController(player);
		local mouse = controller and controller:IsMouseControlled() and 1 or 0;
		digest = (digest * 31 + math.floor(movement.X * 8) * 17 + math.floor(movement.Y * 8) * 3 + mouse) % 2147483629;
	end
	self.seatDigest = digest;
	self.seatQueries = (self.seatQueries or 0) + 1;
end
