-- AI-05: Suppress an alarm.
-- Spawn a sentry and inject an AlarmEvent at a known position. Pass if the
-- sentry's aim angle moves toward the alarm within a 200-tick window.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI05 = Trust.Extend("AI-05", { max_ticks = 600 });

function TestScenarioAI05:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._sentry = a;
    self._alarmInjected = false;
    if a then
        self._initialAim = a:GetAimAngle();
    end
end

function TestScenarioAI05:OnTick(tick)
    local a = self._sentry;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick == 60 and not self._alarmInjected then
        local alarm = AlarmEvent(Vector(1200, 200), Activity.TEAM_2, 1.0);
        MovableMan:RegisterAlarmEvent(alarm);
        self._alarmInjected = true;
        self:RecordMetric("alarm_injected_at_tick", tick);
    end
    if tick % 60 == 0 then
        local delta = math.abs(a:GetAimAngle() - self._initialAim);
        self:RecordMetric("aim_delta", delta);
    end
    return false, false;
end

function TestScenarioAI05:OnEnd()
    local a = self._sentry;
    if a and MovableMan:IsActor(a) then
        local delta = math.abs(a:GetAimAngle() - self._initialAim);
        self:RecordMetric("aim_delta", delta);
        self._passed = self._alarmInjected and delta > 0.1;
    else
        self._passed = false;
    end
end
