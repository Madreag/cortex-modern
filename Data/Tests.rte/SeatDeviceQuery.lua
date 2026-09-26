-- Reads every seat's mouse and device the way a mod does and keeps a running digest of the answers in its fields,
-- so a peer whose answers differ after a restore holds a different script state.
function Create(self)
	self.seatQueries = 0;
	self.seatDigest = 0;
end

function Update(self)
	local activity = ActivityMan:GetActivity();
	local digest = self.seatDigest or 0;
	for player = Activity.PLAYER_1, Activity.MAXPLAYERCOUNT - 1 do
		local movement = UInputMan:GetMouseMovement(player);
		local controller = activity and activity:GetPlayerController(player);
		local mouse = controller and controller:IsMouseControlled() and 1 or 0;
		digest = (digest * 31 + math.floor(movement.X * 8) * 17 + math.floor(movement.Y * 8) * 3 + mouse) % 2147483629;
	end
	self.seatDigest = digest;
	self.seatQueries = (self.seatQueries or 0) + 1;
end
