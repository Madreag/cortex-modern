Two unchanged schedules reproduce an authority-delay floor on the current binary; [exact schedules, line-numbered failures and hashes](quiet-counterexamples.json) are retained.

| Peer RTT | Directory exchange | Delay | Healthy seat wait | Bound |
|---|---|---|---|---|
| 2 ms | 100 ms | 2 ticks | 101 ms | 50 ms |
| 30 ms | 300 ms | 3 ticks | 632 ms | 50 ms |

Only the host process hangs, starting at 6000 ms. In the first schedule the healthy peer needs frame 362 at6034 ms. Even an authority request sent immediately at the fault cannot return before6100 ms, after the50 ms wait deadline at6084 ms. In the second schedule the actual wait begins6048 ms; an ideal first reply at6300 ms would still mean252 ms waiting. The observed632 ms includes a request retry.

The stronger30 ms argument does not depend on the current three-tick choice: the brief permits at most six ticks (100 ms), and100+50 is less than the300 ms directory exchange. The current equal-half contract grants neither peer authority before the directory’s answer. A changed timeout cannot close that gap.

No authority rule, survivor bound, delay cap or fixture has been weakened. The four-seat startup/return failures and input-feel noise failures remain separate unresolved defects. This is local coordinator evidence; spread validation belongs to the lead under this brief.
