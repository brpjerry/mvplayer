# Thin Messages API client for the judge benchmarks: raw HTTP (the project is
# C++, no SDK), the key the app keeps, a response cache so a repeated request
# costs nothing, and a ledger of what every call cost.
import json, os, sys, time, hashlib, urllib.request, urllib.error, threading

KEY_FILE = os.path.expanduser('~/.config/mvplayer/claude-api-key')
CACHE = os.path.expanduser('~/.cache/mvplayer-judge/bench')
PRICE = {'claude-opus-5-5': (4, 20), 'claude-sonnet-5-5': (2, 10), 'claude-haiku-5-5': (0.10, 0.50)}
_lock = threading.Lock()


def key():
    return open(KEY_FILE).read().strip()


def strict(s):
    # Structured outputs need every object closed and fully required.
    if s.get('type') == 'object':
        s = dict(s, additionalProperties=False, required=list(s['properties']),
                 properties={k: strict(v) for k, v in s['properties'].items()})
    elif s.get('type') == 'array':
        s = dict(s, items=strict(s['items']))
    return s


def cost_of(model, usage):
    pin, pout = PRICE.get(model, (0, 0))
    return (usage.get('input_tokens', 0) * pin + usage.get('output_tokens', 0) * pout
            + usage.get('cache_read_input_tokens', 0) * pin * 0.1
            + usage.get('cache_creation_input_tokens', 0) * pin * 1.25) / 1e6


def spent():
    total = 0
    try:
        for l in open(f'{CACHE}/ledger.jsonl'):
            total += json.loads(l)['cost']
    except FileNotFoundError:
        pass
    return total


def ask(model, system, content, schema, effort=None, max_tokens=4000, tag='', cache=True):
    """One structured request. Returns (parsed JSON or {'error': …}, usage, seconds, cached)."""
    body = {'model': model, 'max_tokens': max_tokens, 'system': system,
            'messages': [{'role': 'user', 'content': content}],
            'output_config': {'format': {'type': 'json_schema', 'schema': strict(schema)}}}
    if effort:
        body['output_config']['effort'] = effort
    raw = json.dumps(body, ensure_ascii=False, sort_keys=True).encode()
    h = hashlib.sha256(raw).hexdigest()[:24]
    os.makedirs(f'{CACHE}/responses', exist_ok=True)
    path = f'{CACHE}/responses/{h}.json'
    if cache and os.path.exists(path):
        r = json.load(open(path))
        return r['answer'], r['usage'], r['seconds'], True
    t0 = time.time()
    for attempt in range(6):
        req = urllib.request.Request('https://api.anthropic.com/v1/messages', data=raw, method='POST', headers={
            'content-type': 'application/json', 'x-api-key': key(), 'anthropic-version': '2023-06-01'})
        try:
            with urllib.request.urlopen(req, timeout=300) as resp:
                r = json.load(resp)
            break
        except urllib.error.HTTPError as e:
            text = e.read().decode(errors='replace')
            if e.code in (429, 500, 502, 503, 529) and attempt < 5:
                time.sleep(5 * (attempt + 1))
                continue
            return {'error': f'HTTP {e.code}: {text[:400]}'}, {}, time.time() - t0, False
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            if attempt < 5:
                time.sleep(5 * (attempt + 1))
                continue
            return {'error': f'{e}'}, {}, time.time() - t0, False
    dt = time.time() - t0
    usage = r.get('usage', {})
    with _lock:
        os.makedirs(CACHE, exist_ok=True)
        with open(f'{CACHE}/ledger.jsonl', 'a') as f:
            f.write(json.dumps({'t': time.strftime('%Y-%m-%dT%H:%M:%S'), 'model': model, 'tag': tag,
                                'in': usage.get('input_tokens', 0), 'out': usage.get('output_tokens', 0),
                                'cost': cost_of(model, usage), 'seconds': round(dt, 1)}) + '\n')
    if r.get('stop_reason') != 'end_turn':
        answer = {'error': f"stop_reason {r.get('stop_reason')} {r.get('stop_details') or r.get('error', '')}"}
    else:
        try:
            answer = json.loads(next(b['text'] for b in r['content'] if b['type'] == 'text'))
        except Exception as e:
            answer = {'error': f'{e}: {json.dumps(r)[:400]}'}
    if 'error' not in answer:
        json.dump({'answer': answer, 'usage': usage, 'seconds': round(dt, 1), 'model': model}, open(path, 'w'), ensure_ascii=False)
    return answer, usage, dt, False


if __name__ == '__main__':
    print(f'spent so far: ${spent():.2f}')
