<?php
// Tier 1 sample app. Reachable two ways from fpm-ng.conf's [app] pool:
//   - a real file:      GET /index.php
//   - anything else:    falls through to http.front_controller (this file),
//                        with the original path in PATH_INFO
header('Content-Type: text/plain');
echo "php-fpm-ng combined example\n";
echo "request:   " . ($_SERVER['REQUEST_URI'] ?? '') . "\n";
echo "path_info: " . ($_SERVER['PATH_INFO'] ?? '(none)') . "\n";
echo "time:      " . date('c') . "\n";
