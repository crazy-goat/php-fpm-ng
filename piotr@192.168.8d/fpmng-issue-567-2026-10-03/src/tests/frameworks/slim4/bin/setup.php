<?php

declare(strict_types=1);

$host = getenv('SLIM_DB_HOST') ?: '127.0.0.1';
$port = (int) (getenv('SLIM_DB_PORT') ?: 3306);
$database = getenv('SLIM_DB_NAME') ?: 'slim4';
$user = getenv('SLIM_DB_USER') ?: 'bench';
$password = getenv('SLIM_DB_PASSWORD') ?: 'bench';

if (!preg_match('/^[A-Za-z0-9_]+$/', $database)) {
    fwrite(STDERR, "SLIM_DB_NAME contains unsupported characters\n");
    exit(2);
}

$pdo = new PDO(
    "mysql:host=$host;port=$port;charset=utf8mb4",
    $user,
    $password,
    [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]
);

$pdo->exec("CREATE DATABASE IF NOT EXISTS `$database` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
$pdo->exec("USE `$database`");
$pdo->exec(
    'CREATE TABLE IF NOT EXISTS slim4_probe_items ('
    . 'id INT UNSIGNED NOT NULL PRIMARY KEY, '
    . 'label VARCHAR(64) NOT NULL'
    . ') ENGINE=InnoDB'
);

$statement = $pdo->prepare(
    'INSERT INTO slim4_probe_items (id, label) VALUES (:id, :label) '
    . 'ON DUPLICATE KEY UPDATE label = VALUES(label)'
);
for ($id = 1; $id <= 8; $id++) {
    $statement->execute(['id' => $id, 'label' => "item-$id"]);
}

echo "Slim 4 probe database is ready: $database\n";
