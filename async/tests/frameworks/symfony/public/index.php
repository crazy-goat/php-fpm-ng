<?php

use App\Kernel;
use Symfony\Component\Dotenv\Dotenv;
use Symfony\Component\HttpFoundation\Request;

// Keep the front controller free of symfony/runtime side effects. With shared
// included_files, autoload_runtime.php would be a no-op after request one.
require dirname(__DIR__).'/vendor/autoload.php';

(new Dotenv())->bootEnv(dirname(__DIR__).'/.env');
$environment = $_SERVER['APP_ENV'] ?? getenv('APP_ENV') ?: 'dev';
$debug = filter_var($_SERVER['APP_DEBUG'] ?? getenv('APP_DEBUG') ?: false, FILTER_VALIDATE_BOOL);
$kernel = new Kernel($environment, $debug);
$request = Request::createFromGlobals();
$response = $kernel->handle($request);
$response->send();
$kernel->terminate($request, $response);
