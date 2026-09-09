"""The raw contract-audit fields two independent processes never share.

A continuation candidate is compared against a reference run in another OS process, so every field
written from the real clock differs by construction. Only the fields named here are projected, each
with the source site that makes it per-process; every other field is compared as written.
"""
import re

REAL_CLOCK_FIELDS = (
    ('suffix', '.Timer.m_StartRealTime',
     'Timer::m_StartRealTime is the absolute tick count when the timer was started in real time (Timer.h:282), '
     'taken from g_TimerMan.GetRealTickCount() in Timer::Reset (Timer.cpp:6, Timer.h:78) and read only through '
     'GetElapsedRealTime*/IsPastRealTimeLimit. compare_snapshots masks the same field as real_start in the '
     'cross-process Lua-graph comparison.'),
    ('exact', 'clock.real_time',
     'TimerMan::m_RealTimeTicks counts the microseconds of this process own steady_clock since it started '
     '(TimerMan.h:243, TimerMan.cpp:104). The simulation clock rides clock.sim_count, clock.sim_time and '
     'clock.dt, which are compared as written.'),
    ('exact', 'clock.sim_accumulator',
     'TimerMan::m_SimAccumulator holds the real time not yet chunked into whole DeltaTime steps (TimerMan.h:246), '
     'fed only from real-time increments (TimerMan.cpp:117-129); the simulation advances in whole m_DeltaTime '
     'steps whatever it holds.'),
)


def real_clock_reason(field):
    """The named writer that makes this field per-process, or None for every other field."""
    for kind, name, reason in REAL_CLOCK_FIELDS:
        if field == name if kind == 'exact' else field.endswith(name):
            return reason
    return None


def family(field):
    """One name per field family: a raw path indexes objects, the field it names is the same one."""
    return re.sub(r'\[\d+\]', '[]', field)
