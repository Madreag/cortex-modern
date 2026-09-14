return function(activity, phase, tick)
    local flags, aiTeams, directorTeams = {}, {}, {};
    for team = Activity.TEAM_1, Activity.MAXTEAMCOUNT - 1 do
        if activity:TeamActive(team) then
            local cpu = activity:TeamIsCPU(team);
            table.insert(flags, team .. ":" .. (cpu and 1 or 0));
            if cpu then table.insert(aiTeams, team); end
            if activity.AI and activity.AI[team] then table.insert(directorTeams, team); end
        end
    end
    local function listed(values)
        return #values > 0 and table.concat(values, ",") or "none";
    end
    print("[cpu-facts] phase=" .. phase .. " tick=" .. tick
        .. " state=" .. activity.ActivityState
        .. " running=" .. (activity.ActivityState == Activity.RUNNING and 1 or 0)
        .. " humans=" .. activity.HumanCount .. " legacy=" .. activity.CPUTeam
        .. " aiTeams=" .. listed(aiTeams) .. " directorTeams=" .. listed(directorTeams)
        .. " flags=" .. listed(flags));
end;
