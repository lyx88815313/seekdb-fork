#!/usr/bin/env python3
"""Crash and recovery checks for MiniVectorDB.

The container is driven exactly like the official protocol driver does: one
JSON request per line on stdin, one JSON response per line on stdout. The
container is then killed with SIGKILL to simulate a crash. The /data directory
is reused for the restart, so the test can check three things:

  1. acknowledged commits survive the crash,
  2. a transaction that was never committed does not become visible, not even
     partially,
  3. a crash during the recovery itself leaves the database consistent, in
     other words the recovery is idempotent.

Usage:
  python crash_recovery.py --image minivectordb:local
"""
import argparse
import json
import subprocess
import sys
import tempfile
import time
import uuid


class Server:
    """A single container run against a named volume."""

    def __init__(self, image, volume, name):
        self.image = image
        self.volume = volume
        self.name = name
        self.process = None
        self.sequence = 0
        self.stderr = tempfile.TemporaryFile()

    def start(self):
        argv = [
            'docker', 'run', '--name', self.name, '--interactive',
            '--network=none', '--cpus=2', '--memory=2g', '--memory-swap=2g',
            '--pids-limit=256', '--cap-drop=ALL', '--security-opt=no-new-privileges',
            '--read-only', '--mount', 'type=volume,src=%s,dst=/data' % self.volume,
            self.image,
        ]
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.stderr)
        ready = self.process.stdout.readline().decode('utf-8', 'replace').strip()
        if ready != '{"type":"ready"}':
            raise RuntimeError('unexpected handshake: %r\n%s'
                               % (ready, self.stderr_text()))

    def request(self, sql=None, op='execute', mode='exact'):
        self.sequence += 1
        payload = {'id': self.sequence, 'op': op}
        if sql is not None:
            payload.update(sql=sql, search_mode=mode)
        self.process.stdin.write((json.dumps(payload, ensure_ascii=False) + '\n').encode('utf-8'))
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError('the database exited before the response')
        return json.loads(line.decode('utf-8'))

    def shutdown(self):
        self.request(op='shutdown')
        self.process.stdin.close()
        code = self.process.wait(timeout=30)
        if code != 0:
            raise RuntimeError('the database exited with %d' % code)

    def kill(self):
        """Terminate the process the way a crash does."""
        subprocess.run(['docker', 'kill', '--signal=KILL', self.name],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            pass
        subprocess.run(['docker', 'rm', '--force', self.name],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stderr_text(self):
        try:
            self.stderr.seek(0)
            return self.stderr.read().decode('utf-8', 'replace')
        except (OSError, ValueError):
            return ''


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print('  ok: ' + message)


def start_server(image, volume):
    """Start a container with a unique name against the shared volume."""
    server = Server(image, volume, 'mvdb-crash-run-' + uuid.uuid4().hex[:8])
    server.start()
    return server


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', default='minivectordb:local')
    args = parser.parse_args()

    volume = 'mvdb-crash-' + uuid.uuid4().hex[:12]
    subprocess.run(['docker', 'volume', 'create', volume],
                   stdout=subprocess.DEVNULL, check=True)
    failures = []
    try:
        # ------------------------------------------------------------------
        print('phase 1: acknowledged commit, then a crash with an open transaction')
        server = start_server(args.image, volume)
        server.request('CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(32), embedding VECTOR(2))')
        server.request("INSERT INTO docs VALUES (1, 'kept', '[1,0]'), (2, 'kept', '[0,1]')")
        server.request('BEGIN')
        server.request("INSERT INTO docs VALUES (3, 'rolled back', '[0,0]')")
        server.request("UPDATE docs SET content = 'changed' WHERE id = 1")
        server.kill()

        # ------------------------------------------------------------------
        print('phase 2: restart after the crash')
        server = start_server(args.image, volume)
        rows = server.request('SELECT id, content FROM docs ORDER BY id')
        check(rows['ok'] is True, 'the table survived the crash')
        check([row[0] for row in rows['rows']] == [1, 2],
              'only the committed rows are visible: %r' % (rows['rows'],))
        check(rows['rows'][0][1] == 'kept',
              'the uncommitted update did not become visible: %r' % (rows['rows'][0],))
        again = server.request('SELECT id, content FROM docs ORDER BY id')
        check(again['rows'] == rows['rows'], 'the visible set is stable inside the session')

        # ------------------------------------------------------------------
        print('phase 3: crash while the recovery is running')
        server.kill()
        server = start_server(args.image, volume)
        rows = server.request('SELECT id, content FROM docs ORDER BY id')
        check([row[0] for row in rows['rows']] == [1, 2],
              'the recovery is idempotent: %r' % (rows['rows'],))
        server.kill()

        # ------------------------------------------------------------------
        print('phase 4: crash directly after a commit and before the checkpoint')
        server = start_server(args.image, volume)
        server.request("INSERT INTO docs VALUES (4, 'committed', '[2,2]')")
        server.kill()
        server = start_server(args.image, volume)
        rows = server.request('SELECT id, content FROM docs ORDER BY id')
        check([row[0] for row in rows['rows']] == [1, 2, 4],
              'the acknowledged commit survived the crash: %r' % (rows['rows'],))

        # ------------------------------------------------------------------
        print('phase 5: rollback and a clean shutdown')
        server.request('BEGIN')
        server.request("UPDATE docs SET content = 'temporary' WHERE id = 1")
        server.request('ROLLBACK')
        rows = server.request('SELECT id, content FROM docs ORDER BY id')
        check(rows['rows'][0][1] == 'kept', 'the rollback restored the row: %r' % (rows['rows'][0],))
        server.shutdown()

        server = start_server(args.image, volume)
        rows = server.request('SELECT id, content FROM docs ORDER BY id')
        check([row[0] for row in rows['rows']] == [1, 2, 4],
              'the clean shutdown kept the data: %r' % (rows['rows'],))
        server.shutdown()
    except Exception as error:  # noqa: BLE001 - the report is the interesting part
        failures.append(str(error))
        print('  FAILED: %s' % error)
    finally:
        subprocess.run(['docker', 'volume', 'rm', '-f', volume],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if failures:
        print('crash recovery: FAILED (%d)' % len(failures))
        return 1
    print('crash recovery: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
