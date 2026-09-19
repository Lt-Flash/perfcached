<?php
/**
 * Perfcached.php — the single-file pure-PHP perfcached client
 * (deliverable C).  Requires only ext-sodium and ext-json, both bundled
 * with stock PHP since 7.2.
 *
 * Speaks the text dialect (newline JSON-RPC) over plaintext or the
 * Noise channel (Noise_NNpsk0_25519_ChaChaPoly_SHA256, client
 * principal) - the handshake, Argon2id PSK derivation and transport
 * records reimplement the daemon's contract from the spec, proven
 * against the live responder by test/phptest.sh.
 *
 * Usage:
 *     require 'Perfcached.php';
 *     $pc = new Perfcached('10.0.0.1', 6479,
 *         ['secrets' => ['new-secret', 'old-secret']]);   // list = rotation
 *     $pc->set('sessions', 'user:17', $blob, 300);
 *     $v = $pc->get('sessions', 'user:17');               // null on miss
 *     $n = $pc->add('counters', 'calls');
 *
 * Values are binary-safe both directions (the base64 leg of the wire is
 * handled internally).  Misses return null/false per method; transport
 * and server errors throw PerfcachedException with the daemon's
 * message.  One instance per request/process - not thread- or
 * fiber-shared (the hiredis contract, same as libperfd).
 */

final class PerfcachedException extends \RuntimeException
{
}

final class Perfcached
{
    /** The client's version - semver, independent of the daemon's. */
    public const VERSION = '0.2.0';

    private const MAXPT = 65519;           /* Noise record plaintext cap */

    /** @var resource */
    private $sock;
    private bool $encrypted = false;
    private string $sendK = '', $recvK = '';
    private int $sendN = 0, $recvN = 0;
    private string $rbuf = '';
    private int $id = 0;

    /**
     * @param array{secrets?: string[], timeout?: float, unix?: string} $opts
     *   secrets: tried in order on fresh connections (rotation =
     *            add-new/drain-old); empty/absent = plaintext.
     *   timeout: seconds, connect and IO (default 5.0).
     *   unix:    a socket path; $host/$port are then ignored.
     */
    public function __construct(string $host, int $port = 6479,
        array $opts = [])
    {
        $timeout = (float)($opts['timeout'] ?? 5.0);
        $secrets = $opts['secrets'] ?? [];
        $target = isset($opts['unix'])
            ? 'unix://' . $opts['unix']
            : 'tcp://' . $host . ':' . $port;

        if ($secrets === []) {
            $this->sock = $this->dial($target, $timeout);
            return;
        }
        $last = 'no secrets tried';
        foreach ($secrets as $secret) {
            $this->sock = $this->dial($target, $timeout);
            try {
                $this->handshake($secret);
                return;
            } catch (PerfcachedException $e) {
                $last = $e->getMessage();
                fclose($this->sock);
                $this->encrypted = false;
            }
        }
        throw new PerfcachedException(
            'handshake failed with every secret: ' . $last);
    }

    public function close(): void
    {
        if (is_resource($this->sock)) {
            fclose($this->sock);
        }
    }

    /* ---- typed verbs --------------------------------------------------- */

    public function ping(): void
    {
        $r = $this->command('ping');
        if (!($r['pong'] ?? false)) {
            throw new PerfcachedException('bad pong');
        }
    }

    public function set(string $col, string $key, string $value,
        int $ttl = 0): void
    {
        $p = ['col' => $col, 'key' => $key] + self::encodeValue($value);
        if ($ttl > 0) {
            $p['ttl'] = $ttl;
        }
        $r = $this->command('set', $p);
        if (!($r['stored'] ?? false)) {
            throw new PerfcachedException('not stored');
        }
    }

    /** null on miss */
    public function get(string $col, string $key): ?string
    {
        $r = $this->command('get', ['col' => $col, 'key' => $key]);
        if (!($r['found'] ?? false)) {
            return null;
        }
        return self::decodeValue($r);
    }

    public function del(string $col, string $key): bool
    {
        $r = $this->command('del', ['col' => $col, 'key' => $key]);
        return (bool)($r['deleted'] ?? false);
    }

    public function exists(string $col, string $key): bool
    {
        $r = $this->command('exists', ['col' => $col, 'key' => $key]);
        return (bool)($r['exists'] ?? false);
    }

    /** seconds left; -1 = no expiry; -2 = no such key */
    public function ttl(string $col, string $key): int
    {
        $r = $this->command('ttl', ['col' => $col, 'key' => $key]);
        return (int)($r['ttl'] ?? -2);
    }

    /** true = re-armed, false = no such key */
    public function expire(string $col, string $key, int $ttl): bool
    {
        $r = $this->command('expire',
            ['col' => $col, 'key' => $key, 'ttl' => $ttl]);
        return (bool)($r['updated'] ?? false);
    }

    public function add(string $col, string $key, int $by = 1,
        int $ttl = 0): int
    {
        $p = ['col' => $col, 'key' => $key, 'by' => $by];
        if ($ttl > 0) {
            $p['ttl'] = $ttl;
        }
        return (int)$this->command('add', $p)['value'];
    }

    public function sub(string $col, string $key, int $by = 1): int
    {
        return (int)$this->command('sub',
            ['col' => $col, 'key' => $key, 'by' => $by])['value'];
    }

    /** @param string[] $keys  @return array<string, ?string> key => value */
    public function mget(string $col, array $keys): array
    {
        $r = $this->command('mget', ['col' => $col, 'keys' =>
            array_values($keys)]);
        $out = [];
        foreach (array_values($keys) as $i => $k) {
            $e = $r['values'][$i] ?? ['found' => false];
            $out[$k] = ($e['found'] ?? false) ? self::decodeValue($e) : null;
        }
        return $out;
    }

    /** @return string[] */
    public function keys(string $col, ?string $match = null,
        int $limit = 0): array
    {
        $p = ['col' => $col];
        if ($match !== null) {
            $p['match'] = $match;
        }
        if ($limit > 0) {
            $p['limit'] = $limit;
        }
        $out = [];
        foreach ($this->command('keys', $p)['keys'] ?? [] as $k) {
            /* binary keys arrive as {"b64": ...} */
            $out[] = is_array($k) ? base64_decode($k['b64'], true) : $k;
        }
        return $out;
    }

    /** raw JSON fragment at $path, or null on miss */
    public function jget(string $col, string $key, string $path = '$'): ?string
    {
        $r = $this->command('jget',
            ['col' => $col, 'key' => $key, 'path' => $path]);
        if (!($r['found'] ?? false)) {
            return null;
        }
        return json_encode($r['value'], JSON_UNESCAPED_SLASHES |
            JSON_UNESCAPED_UNICODE);
    }

    /** $jsonValue is RAW JSON (objects, arrays, numbers, strings) */
    public function jset(string $col, string $key, string $path,
        string $jsonValue, int $ttl = 0): void
    {
        $val = json_decode($jsonValue, true, 512, JSON_THROW_ON_ERROR);
        $p = ['col' => $col, 'key' => $key, 'path' => $path, 'val' => $val];
        if ($ttl > 0) {
            $p['ttl'] = $ttl;
        }
        $r = $this->command('jset', $p);
        if (!($r['set'] ?? false)) {
            throw new PerfcachedException('not set');
        }
    }

    public function jdel(string $col, string $key, string $path): bool
    {
        $r = $this->command('jdel',
            ['col' => $col, 'key' => $key, 'path' => $path]);
        return (bool)($r['deleted'] ?? false);
    }

    public function jincr(string $col, string $key, string $path,
        int $by = 1): int
    {
        return (int)$this->command('jincr',
            ['col' => $col, 'key' => $key, 'path' => $path,
             'by' => $by])['value'];
    }

    /** @return array the decoded stats document */
    public function stats(?string $col = null): array
    {
        return $this->command('stats',
            $col !== null ? ['col' => $col] : null);
    }

    /**
     * The escape hatch: any method + params.  Returns the decoded
     * result member; throws with the server's message on an error
     * member.
     */
    public function command(string $method, ?array $params = null): array
    {
        $req = ['jsonrpc' => '2.0', 'id' => ++$this->id,
            'method' => $method];
        if ($params !== null) {
            $req['params'] = $params;
        }
        $this->sendBytes(json_encode($req,
            JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . "\n");
        for (;;) {
            $line = $this->recvLine();
            $msg = json_decode($line, true);
            if (!is_array($msg) || !array_key_exists('id', $msg)) {
                continue;                  /* notification: skip (S7) */
            }
            if ((int)$msg['id'] !== $this->id) {
                continue;                  /* stale/foreign: skip */
            }
            if (isset($msg['error'])) {
                throw new PerfcachedException(
                    $msg['error']['message'] ?? 'server error');
            }
            return is_array($msg['result']) ? $msg['result']
                : ['value' => $msg['result']];
        }
    }

    /* ---- value codec (decision #1: clean UTF-8 plain, else base64) ----- */

    /** @return array{value: string, enc?: string} */
    private static function encodeValue(string $v): array
    {
        if (strpos($v, "\0") === false && preg_match('//u', $v) === 1) {
            return ['value' => $v];
        }
        return ['value' => base64_encode($v), 'enc' => 'b64'];
    }

    private static function decodeValue(array $r): string
    {
        $v = (string)($r['value'] ?? '');
        if (($r['enc'] ?? null) === 'b64') {
            $d = base64_decode($v, true);
            if ($d === false) {
                throw new PerfcachedException('bad b64 value');
            }
            return $d;
        }
        return $v;
    }

    /* ---- transport ----------------------------------------------------- */

    /** @return resource */
    private function dial(string $target, float $timeout)
    {
        $sock = @stream_socket_client($target, $errno, $errstr, $timeout);
        if ($sock === false) {
            throw new PerfcachedException(
                "cannot connect $target: $errstr");
        }
        stream_set_timeout($sock, (int)$timeout,
            (int)(($timeout - (int)$timeout) * 1e6));
        return $sock;
    }

    private function ioWrite(string $b): void
    {
        for ($off = 0, $n = strlen($b); $off < $n;) {
            $w = @fwrite($this->sock, substr($b, $off));
            if ($w === false || $w === 0) {
                throw new PerfcachedException('connection failed (write)');
            }
            $off += $w;
        }
    }

    private function ioRead(int $n): string
    {
        $b = '';
        while (strlen($b) < $n) {
            $d = @fread($this->sock, $n - strlen($b));
            if ($d === false || $d === '') {
                throw new PerfcachedException('connection closed');
            }
            $b .= $d;
        }
        return $b;
    }

    private function sendBytes(string $b): void
    {
        if (!$this->encrypted) {
            $this->ioWrite($b);
            return;
        }
        for ($off = 0, $n = strlen($b); $off < $n; $off += self::MAXPT) {
            $chunk = substr($b, $off, self::MAXPT);
            $ct = sodium_crypto_aead_chacha20poly1305_ietf_encrypt(
                $chunk, '', self::nonce($this->sendN++), $this->sendK);
            $this->ioWrite(pack('v', strlen($ct)) . $ct);
        }
    }

    private function recvLine(): string
    {
        for (;;) {
            $nl = strpos($this->rbuf, "\n");
            if ($nl !== false) {
                $line = substr($this->rbuf, 0, $nl);
                $this->rbuf = substr($this->rbuf, $nl + 1);
                return $line;
            }
            if (!$this->encrypted) {
                $d = @fread($this->sock, 65536);
                if ($d === false || $d === '') {
                    throw new PerfcachedException('connection closed');
                }
                $this->rbuf .= $d;
                continue;
            }
            $len = unpack('v', $this->ioRead(2))[1];
            if ($len < 16 || $len > 65535) {
                throw new PerfcachedException('bad transport record');
            }
            $pt = sodium_crypto_aead_chacha20poly1305_ietf_decrypt(
                $this->ioRead($len), '', self::nonce($this->recvN++),
                $this->recvK);
            if ($pt === false) {
                throw new PerfcachedException('transport decrypt failed');
            }
            $this->rbuf .= $pt;
        }
    }

    /* ---- Noise_NNpsk0_25519_ChaChaPoly_SHA256 (client principal) ------- */

    private static function nonce(int $n): string
    {
        return "\0\0\0\0" . pack('P', $n);
    }

    private static function psk(string $password): string
    {
        $salt = substr(hash('sha256',
            'perfcached-psk-salt-v1:client', true), 0,
            SODIUM_CRYPTO_PWHASH_SALTBYTES);
        return sodium_crypto_pwhash(32, $password, $salt,
            SODIUM_CRYPTO_PWHASH_OPSLIMIT_INTERACTIVE,
            SODIUM_CRYPTO_PWHASH_MEMLIMIT_INTERACTIVE,
            SODIUM_CRYPTO_PWHASH_ALG_ARGON2ID13);
    }

    /** @return string[] two or three 32-byte outputs */
    private static function hkdf(string $ck, string $ikm, int $n): array
    {
        $tk = hash_hmac('sha256', $ikm, $ck, true);
        $o1 = hash_hmac('sha256', "\x01", $tk, true);
        if ($n === 1) {
            return [$o1];
        }
        $o2 = hash_hmac('sha256', $o1 . "\x02", $tk, true);
        if ($n === 2) {
            return [$o1, $o2];
        }
        return [$o1, $o2, hash_hmac('sha256', $o2 . "\x03", $tk, true)];
    }

    private function handshake(string $secret): void
    {
        $name = 'Noise_NNpsk0_25519_ChaChaPoly_SHA256';
        $h = strlen($name) > 32 ? hash('sha256', $name, true)
            : str_pad($name, 32, "\0");
        $ck = $h;
        $k = null;
        $kn = 0;

        $mixHash = function (string $d) use (&$h): void {
            $h = hash('sha256', $h . $d, true);
        };
        $mixKey = function (string $ikm) use (&$ck, &$k, &$kn): void {
            [$ck, $k] = self::hkdf($ck, $ikm, 2);
            $kn = 0;
        };

        /* prologue = the key-id byte (client principal = 0) */
        $mixHash("\0");
        /* psk0: mix_key_and_hash(psk) */
        [$ck, $th, $k] = self::hkdf($ck, self::psk($secret), 3);
        $mixHash($th);
        $kn = 0;

        /* -> e (+ encrypted payload: the client version byte) */
        $kp = sodium_crypto_box_keypair();
        $ePriv = sodium_crypto_box_secretkey($kp);
        $ePub = sodium_crypto_box_publickey($kp);
        $mixHash($ePub);
        $mixKey($ePub);
        $ct1 = sodium_crypto_aead_chacha20poly1305_ietf_encrypt(
            "\x01", $h, self::nonce($kn++), $k);
        $mixHash($ct1);
        $noise1 = $ePub . $ct1;
        $this->ioWrite(pack('v', 1 + strlen($noise1)) . "\0" . $noise1);

        /* <- e, ee (a rejected handshake just closes the socket) */
        try {
            $len = unpack('v', $this->ioRead(2))[1];
            $msg2 = $this->ioRead($len);
        } catch (PerfcachedException $e) {
            throw new PerfcachedException(
                'handshake refused (wrong secret?)');
        }
        if (strlen($msg2) < 32 + 16) {
            throw new PerfcachedException('short handshake reply');
        }
        $re = substr($msg2, 0, 32);
        $mixHash($re);
        $mixKey($re);
        $mixKey(sodium_crypto_scalarmult($ePriv, $re));
        $pt = sodium_crypto_aead_chacha20poly1305_ietf_decrypt(
            substr($msg2, 32), $h, self::nonce($kn), $k);
        if ($pt === false) {
            throw new PerfcachedException('handshake failed (wrong secret?)');
        }
        [$this->sendK, $this->recvK] = self::hkdf($ck, '', 2);
        $this->sendN = $this->recvN = 0;
        $this->encrypted = true;
        sodium_memzero($ePriv);
    }
}
