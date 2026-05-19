.symopt+0x40
.reload /f
.lines -e
.echo ===MAIN_REGS===
~0 r
.echo ===MAIN_KN===
~0 kn 30
.echo ===MAIN_DV===
~0 dv /v
.echo ===CALL_SITE_5b35dd===
.echo bytes around the Main::Update call site that calls the lock primitive:
u SkyrimSE+0x5b35d0 L20
.echo ===CALL_SITE_5765ff===
.echo bytes around the lock primitive entry / wait:
u SkyrimSE+0x5765d0 L20
.echo ===RENDER_REGS===
~~[1df4.6bd4] r
.echo ===RENDER_KN===
~~[1df4.6bd4] kn 30
.echo ===WORKER_TID7060_REGS===
~~[1df4.7060] r
.echo ===WORKER_TID7060_KN===
~~[1df4.7060] kn 30
.echo ===WORKER_TID251c_REGS===
~~[1df4.251c] r
.echo ===WORKER_TID251c_KN===
~~[1df4.251c] kn 30
.echo ===CALL_SITE_6d468a===
.echo bytes for the JOB BODY (4 workers' common job):
u SkyrimSE+0x6d4620 L40
.echo ===CALL_SITE_6ef480===
u SkyrimSE+0x6ef230 L40
.echo ===PASS2_DONE===
q
