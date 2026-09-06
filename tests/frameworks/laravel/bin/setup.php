<?php

declare(strict_types=1);

$host = getenv('LARAVEL_DB_HOST') ?: '127.0.0.1';
$port = (int) (getenv('LARAVEL_DB_PORT') ?: 3306);
$database = getenv('LARAVEL_DB_NAME') ?: 'laravel025';
$user = getenv('LARAVEL_DB_USER') ?: 'bench';
$password = getenv('LARAVEL_DB_PASSWORD') ?: 'bench';

if (!preg_match('/^[A-Za-z0-9_]+$/', $database)) {
    fwrite(STDERR, "LARAVEL_DB_NAME contains unsupported characters\n");
    exit(2);
}

$pdo = new PDO(
    "mysql:host=$host;port=$port;charset=utf8mb4",
    $user,
    $password,
    [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_EMULATE_PREPARES => false,
    ],
);

try {
    $pdo->exec("CREATE DATABASE IF NOT EXISTS `$database` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
} catch (PDOException $exception) {
    if (!str_contains($exception->getMessage(), '1044')) {
        throw $exception;
    }
    // A least-privilege probe account may use its private database without CREATE DATABASE privilege.
}
$pdo->exec("USE `$database`");
$pdo->exec(
    'CREATE TABLE IF NOT EXISTS users ('
    . 'id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY, '
    . 'name VARCHAR(255) NOT NULL UNIQUE, '
    . 'email VARCHAR(255) NOT NULL UNIQUE, '
    . 'email_verified_at TIMESTAMP NULL, '
    . 'password VARCHAR(255) NOT NULL, '
    . 'remember_token VARCHAR(100) NULL, '
    . 'created_at TIMESTAMP NULL, '
    . 'updated_at TIMESTAMP NULL'
    . ') ENGINE=InnoDB',
);
$pdo->exec(
    'CREATE TABLE IF NOT EXISTS probe_items ('
    . 'id INT UNSIGNED NOT NULL PRIMARY KEY, '
    . 'label VARCHAR(64) NOT NULL'
    . ') ENGINE=InnoDB',
);

$userStatement = $pdo->prepare(
    'INSERT INTO users (name, email, password, created_at, updated_at) '
    . 'VALUES (:name, :email, :password, NOW(), NOW()) '
    . 'ON DUPLICATE KEY UPDATE email = VALUES(email), updated_at = NOW()',
);
foreach (['alice', 'bob'] as $name) {
    $userStatement->execute([
        'name' => $name,
        'email' => $name.'@laravel025.test',
        'password' => password_hash('probe-password', PASSWORD_BCRYPT, ['cost' => 4]),
    ]);
}

$itemStatement = $pdo->prepare(
    'INSERT INTO probe_items (id, label) VALUES (:id, :label) '
    . 'ON DUPLICATE KEY UPDATE label = VALUES(label)',
);
for ($id = 1; $id <= 8; $id++) {
    $itemStatement->execute(['id' => $id, 'label' => "item-$id"]);
}

echo "Laravel 13.30.1 probe database is ready: $database\n";
