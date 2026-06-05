import sys, json, base64
raw = sys.stdin.read()
parts = raw.split('\n===SLOT===\n', 1)
try:
    reg = json.loads(parts[0].strip()) if parts[0].strip() else {}
    slot = json.loads(parts[1].strip()) if len(parts) > 1 and parts[1].strip() else {}
    kvs = reg.get('kvs', [])
    cnt = len(kvs)
    orphans = sum(1 for kv in kvs if kv.get('lease', '0') == '0')
    nodes = []
    errs = []
    for kv in kvs:
        val = json.loads(base64.b64decode(kv.get('value', '')))
        nid = val.get('node_id', 0)
        ntype = val.get('node_type', '?')
        nip = val.get('node_ip', '')
        nport = val.get('node_port', 0)
        lease = kv.get('lease', '0')
        nodes.append({'nid': nid, 'type': ntype, 'addr': f'{nip}:{nport}', 'lease': lease})
    nid_vals = [n['nid'] for n in nodes]
    status = 1
    if cnt < 3:
        status = 0
        errs.append(f'node_count={cnt}(<3)')
    if orphans > 0:
        status = 0
        errs.append(f'orphan_keys={orphans}')
    if len(nid_vals) != len(set(nid_vals)):
        status = 0
        errs.append('duplicate_node_id')
    if any(n < 1 or n > 255 for n in nid_vals):
        status = 0
        errs.append('node_id_oob')
    slot_ips = set()
    for kv in slot.get('kvs', []):
        slot_ips.add(base64.b64decode(kv.get('value', '')).decode())
    reg_ips = set()
    for kv in kvs:
        reg_ips.add(base64.b64decode(kv['key']).decode()[len('/thunder/registry/'):])
    if slot_ips and reg_ips and slot_ips != reg_ips:
        status = 0
        errs.append('slot_registry_mismatch')
    types = sorted(set(n['type'] for n in nodes))
    nids_sorted = [str(n['nid']) for n in sorted(nodes, key=lambda x: x['nid'])]
    lease_ids = sorted(set(n['lease'] for n in nodes))
    print(f'OK={status} nodes={cnt} types={",".join(types)} nids={",".join(nids_sorted)} leases={len(lease_ids)} orphans={orphans} errs={";".join(errs) if errs else "none"}')
    for n in sorted(nodes, key=lambda x: x['nid']):
        print(f'  node_id={n["nid"]:>3}  type={n["type"]:<10}  addr={n["addr"]:<22}  lease={n["lease"]}')
except Exception as e:
    print(f'OK=0 nodes=0 types=? nids=? leases=0 orphans=0 errs=parse_error:{e}')
