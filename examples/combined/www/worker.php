<?php
// supervisor.script (fpm_pool_script.c): a long-lived worker, run once per
// process. Never returns under normal operation -- if it does exit (crash,
// kill -9), supervisor.restart governs whether/when it comes back
// (fpm_pool_supervisor.c). Heartbeat file lets pool.type = status and this
// example's verification script both see it's alive without touching FPM's
// own shared memory.
while (true) {
    file_put_contents('/www/data/worker-heartbeat.txt', getmypid() . ' ' . date('c') . "\n");
    sleep(2);
}
