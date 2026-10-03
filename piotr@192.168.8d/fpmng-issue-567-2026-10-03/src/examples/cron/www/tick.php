<?php
// Run once per schedule hit (cron.schedule below), in-process, then this
// process exits and pm=static/1 respawns it to wait for the next occurrence
// (fpm_pool_cron.c). Writes its own marker line; cron.log (fpm-ng.conf)
// separately records fpm's own start/exit/duration view of the same run.
file_put_contents('/www/data/tick.txt', date('c') . " tick pid=" . getmypid() . "\n", FILE_APPEND);
