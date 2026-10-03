<?php
// cron.script (fpm_pool_script.c): run once per schedule hit, in-process,
// then the process exits and the pm=static/1 machinery respawns it to wait
// for the next occurrence (fpm_pool_cron.c). Writes its own marker line so
// the tick is visible to something other than fpm's own cron.log.
file_put_contents('/www/data/cron-tick.txt', date('c') . " tick pid=" . getmypid() . "\n", FILE_APPEND);
