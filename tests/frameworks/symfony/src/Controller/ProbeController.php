<?php

namespace App\Controller;

use App\Entity\Item;
use App\Probe\Gate;
use Doctrine\DBAL\Connection;
use Doctrine\ORM\EntityManagerInterface;
use Symfony\Bundle\FrameworkBundle\Controller\AbstractController;
use Symfony\Component\Cache\Adapter\RedisAdapter;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\RequestStack;
use Symfony\Component\HttpKernel\KernelInterface;
use Symfony\Component\Routing\Attribute\Route;
use Symfony\Contracts\Cache\CacheInterface;

final class ProbeController extends AbstractController
{
    // This value is diagnostic: Symfony does not need fiber.isolate_statics.
    public static int $hits = 0;

    private static ?string $lastUser = null;

    #[Route('/health', name: 'health')]
    public function health(): JsonResponse
    {
        return new JsonResponse(['ok' => true]);
    }

    #[Route('/mix', name: 'mix')]
    public function mix(
        Request $request,
        Connection $connection,
        EntityManagerInterface $entityManager,
        CacheInterface $cache,
        RequestStack $stack,
        KernelInterface $kernel,
    ): JsonResponse {
        $id = (int) $request->query->get('id', 0);
        $sleep = (float) $request->query->get('sleep', 0);
        $token = bin2hex(random_bytes(4));
        $redisKey = (string) ($_SERVER['REDIS_PREFIX'] ?? 'fpmng:symfony:default:').'probe:'.$id.':'.$token;
        $redis = RedisAdapter::createConnection((string) ($_SERVER['REDIS_DSN'] ?? 'redis://127.0.0.1:6379/15'));
        $redis->set($redisKey, 'v-'.$id.'-'.$token);
        $redis->expire($redisKey, 60);
        $cacheValue = $cache->get('probe.'.$id.'.'.$token, static fn (): string => 'c-'.$id.'-'.$token);
        $row = $connection->fetchAssociative(
            'SELECT SLEEP(?) AS waited, CONNECTION_ID() AS connection_id, ? AS tag',
            [$sleep, 't-'.$id.'-'.$token],
        );
        $item = $entityManager->find(Item::class, $id);
        $redisValue = $redis->get($redisKey);

        return new JsonResponse([
            'id' => $id,
            'token' => $token,
            'item' => $item?->label,
            'db_tag' => $row['tag'] ?? null,
            'db_connection_id' => (int) ($row['connection_id'] ?? 0),
            'redis' => $redisValue,
            'cache' => $cacheValue,
            'ok' => $item?->label === 'item-'.$id
                && $redisValue === 'v-'.$id.'-'.$token
                && $cacheValue === 'c-'.$id.'-'.$token
                && ($row['tag'] ?? null) === 't-'.$id.'-'.$token,
        ] + $this->common($request, $stack, $kernel));
    }

    #[Route('/session', name: 'session')]
    public function session(
        Request $request,
        Gate $gate,
        RequestStack $stack,
        KernelInterface $kernel,
    ): JsonResponse {
        $user = (string) $request->query->get('user', 'anon');
        $session = $request->getSession();
        $before = $session->get('user');
        $gateReleased = $gate->wait(
            (string) $request->query->get('gate', ''),
            $user.'-'.bin2hex(random_bytes(3)),
        );
        if ($before === null) {
            $session->set('user', $user);
        }
        $count = (int) $session->get('count', 0) + 1;
        $session->set('count', $count);

        return new JsonResponse([
            'user_param' => $user,
            'sess_user_before' => $before,
            'sess_user' => $session->get('user'),
            'count' => $count,
            'sid' => $session->getId(),
            'gate_released' => $gateReleased,
            'ok' => $session->get('user') === $user && $gateReleased,
            'handler' => ini_get('session.save_handler'),
        ] + $this->common($request, $stack, $kernel));
    }

    #[Route('/me', name: 'me')]
    public function me(
        Request $request,
        Gate $gate,
        RequestStack $stack,
        KernelInterface $kernel,
    ): JsonResponse {
        $user = $this->getUser()?->getUserIdentifier();
        $gateReleased = $gate->wait(
            (string) $request->query->get('gate', ''),
            (string) ($user ?? 'anonymous').'-'.bin2hex(random_bytes(3)),
        );
        $previousStaticUser = self::$lastUser;
        self::$lastUser = $user;

        return new JsonResponse([
            'user' => $user,
            'prev_static_user' => $previousStaticUser,
            'auth_header' => $request->headers->has('Authorization') ? 'yes' : 'no',
            'gate_released' => $gateReleased,
            'ok' => $gateReleased && $user !== null,
        ] + $this->common($request, $stack, $kernel));
    }

    #[Route('/identity', name: 'identity')]
    public function identity(
        Request $request,
        Gate $gate,
        EntityManagerInterface $entityManager,
        Connection $connection,
        RequestStack $stack,
        KernelInterface $kernel,
    ): JsonResponse {
        $gateReleased = $gate->wait(
            (string) $request->query->get('gate', ''),
            bin2hex(random_bytes(6)),
        );

        return new JsonResponse([
            'ok' => $gateReleased,
            'gate_released' => $gateReleased,
            'entity_manager_oid' => spl_object_id($entityManager),
            'connection_oid' => spl_object_id($connection),
            'user' => $this->getUser()?->getUserIdentifier(),
        ] + $this->common($request, $stack, $kernel));
    }

    #[Route('/who', name: 'who')]
    public function who(
        Request $request,
        RequestStack $stack,
        KernelInterface $kernel,
    ): JsonResponse {
        return new JsonResponse([
            'user' => $this->getUser()?->getUserIdentifier(),
            'leaked_static_user' => self::$lastUser,
        ] + $this->common($request, $stack, $kernel));
    }

    private function common(Request $request, RequestStack $stack, KernelInterface $kernel): array
    {
        self::$hits++;

        return [
            'pid' => getmypid(),
            'uri' => $request->server->get('REQUEST_URI'),
            'hits_static' => self::$hits,
            'kernel_oid' => spl_object_id($kernel),
            'container_oid' => spl_object_id($this->container),
            'request_oid' => spl_object_id($request),
            'stack_depth' => $stack->getParentRequest() === null ? 1 : 2,
            'memory_bytes' => memory_get_usage(true),
            'included_files' => count(get_included_files()),
            'output_buffer_level' => ob_get_level(),
            'environment' => $kernel->getEnvironment(),
        ];
    }
}
