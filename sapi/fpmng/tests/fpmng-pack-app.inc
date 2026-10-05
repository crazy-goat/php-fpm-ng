<?php

/* Shared by the fpmng-pack*.phpt tests (issues #429, #430). The test PHP may
 * run with phar.readonly=1, and the PHAR must not come from the code under test,
 * so a small uncompressed, SHA-256-signed PHAR is written out by hand. */

/** @param array<string,string> $files entry name => contents */
function fpmng_mini_phar(array $files, string $stub = "<?php\n__HALT_COMPILER(); ?>\r\n"): string
{
    $entries = '';
    $data = '';
    foreach ($files as $name => $body) {
        $entries .= pack('V', strlen($name)) . $name
            . pack('VVVVVV', strlen($body), 0, strlen($body), crc32($body), 0x1B6, 0);
        $data .= $body;
    }
    $manifest = pack('VnVV', count($files), 0x1100, 0x10000, 0) . pack('V', 0) . $entries;
    $phar = $stub . pack('V', strlen($manifest)) . $manifest . $data;
    return $phar . hash('sha256', $phar, true) . pack('V', 3) . 'GBMB';
}

/** Writes the three inputs, packs them into $out, then removes the inputs. */
function fpmng_pack(string $binary, string $dir, string $phar, string $ini, string $conf, string $out): void
{
    file_put_contents("$dir/in.phar", $phar);
    file_put_contents("$dir/in.ini", $ini);
    file_put_contents("$dir/in.conf", $conf);
    exec(escapeshellarg($binary) . ' pack ' . escapeshellarg("$dir/in.phar") . ' ' . escapeshellarg("$dir/in.ini")
        . ' ' . escapeshellarg("$dir/in.conf") . ' -o ' . escapeshellarg($out) . ' 2>&1', $lines, $status);
    if ($status !== 0) {
        throw new RuntimeException('pack failed: ' . implode("\n", $lines));
    }
    unlink("$dir/in.phar");
    unlink("$dir/in.ini");
    unlink("$dir/in.conf");
}
