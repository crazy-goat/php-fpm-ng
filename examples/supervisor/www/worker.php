<?php
// A long-lived worker (supervisor.script below), run once per process.
// Never returns under normal operation. If it exits -- crash, or `kill -9`
// for this example's restart demonstration -- supervisor.restart governs
// whether/when it comes back (fpm_pool_supervisor.c).
while (true) {
    file_put_contents('/www/data/heartbeat.txt', getmypid() . ' ' . date('c') . "\n");
    sleep(2);
}
