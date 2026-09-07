<?php
header('Content-Type: text/plain');
echo "php-fpm-ng http example\n";
echo "scheme:  " . (($_SERVER['HTTPS'] ?? '') !== '' ? 'https' : 'http') . "\n";
echo "request: " . ($_SERVER['REQUEST_URI'] ?? '') . "\n";
