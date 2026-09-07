function OnSave(self)
end

function Destroy(self)
	local report = MovableMan:FindObjectByUniqueID(self:GetNumberValue("purge_report"));
	assert(report, "purge deleted an object owned outside MovableMan");
	report:SetNumberValue("purge_callbacks", report:GetNumberValue("purge_callbacks") + 1);
	local added = self:Clone();
	local kind = self:GetNumberValue("purge_kind");
	report:SetNumberValue("purge_spawn_" .. kind, added.UniqueID);
	if kind < 2 then
		MovableMan:AddActor(added);
	elseif kind == 2 then
		MovableMan:AddItem(added);
	else
		MovableMan:AddParticle(added);
	end
	if kind == 0 then
		-- Re-entry from a mod callback must join the purge already in progress.
		MovableMan:PurgeAllMOs();
	end
end
