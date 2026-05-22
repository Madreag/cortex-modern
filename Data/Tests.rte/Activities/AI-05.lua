-- AI-05: Respond to an alarm event.
-- An alarm is injected to the actor's right at tick 60. Pass: the actor genuinely
-- advances toward it (>= 150px from its landed position) -- a real response. A
-- sentry that just turns to face the alarm but holds position fails. The
-- self-test walks the actor toward the alarm. Result locks in OnTick.

package.loaded.Constants = nil; require("Constants");
local Trust = require("Lib/TrustScenario");

TestScenarioAI05 = Trust.Extend("AI-05", { max_ticks = 600 });

function TestScenarioAI05:OnStart()
    local a = self:SpawnActor("Green Dummy", "Base.rte", 950, 50, Activity.TEAM_1, Actor.AIMODE_SENTRY);
    self._actor = a;
    self._alarmPos = Vector(1250, 355);
    self._alarmInjected = false;
    self._landedX = nil;
end

function TestScenarioAI05:OnTick(tick)
    local a = self._actor;
    if not a or not MovableMan:IsActor(a) then
        return true, false;
    end
    if tick == 90 and not self._landedX then
        self._landedX = a.Pos.X;
    end
    if tick == 60 and not self._alarmInjected then
        local alarm = AlarmEvent(self._alarmPos, Activity.TEAM_2, 1.0);
        MovableMan:RegisterAlarmEvent(alarm);
        self._alarmInjected = true;
        self:RecordMetric("alarm_injected_at_tick", tick);
    end
    -- Self-test: genuinely move the actor toward the alarm.
    if tick == 120 and self._selfTest and self._landedX then
        a.Pos = Vector(self._landedX + 200, a.Pos.Y);
    end
    if self._alarmInjected and self._landedX then
        local advance = a.Pos.X - self._landedX;
        if tick % 60 == 0 then
            self:RecordMetric("advance_toward_alarm", advance);
        end
        if advance >= 150 then
            self:RecordMetric("advance_toward_alarm", advance);
            self:RecordMetric("responded_tick", tick);
            return true, true;
        end
        if tick > 450 then
            self:RecordMetric("advance_toward_alarm", advance);
            return true, false;
        end
    end
    return false, false;
end

function TestScenarioAI05:OnEnd()
end
