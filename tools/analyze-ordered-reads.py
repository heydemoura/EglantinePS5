#!/usr/bin/env python3
"""Summarize every captured ordered read by actual guest-frame time boundaries.

The per-frame overlap totals sum CPU-thread time, so may exceed frame wall time.
A clean cache at backend service is an observation, never a CPU-side bypass proof.
"""
import argparse
import bisect
import collections
import csv
import json
from pathlib import Path

CALLERS = {0: 'unclassified', 1: 'ReadBlock', 2: 'CopyBlock', 3: 'InvalidateDataCache', 4: 'ScalarRead'}


def percentile(values, fraction):
    if not values:
        return 0
    v = sorted(values)
    return v[min(len(v) - 1, int((len(v) - 1) * fraction + 0.5))]


def write_csv(path, rows):
    with path.open('w', newline='') as out:
        if rows:
            writer = csv.DictWriter(out, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)


def analyze(prefix, allow_incomplete_copy_sources=False):
    meta = dict(line.split('=', 1) for line in Path(str(prefix) + '-meta.txt').read_text().splitlines() if '=' in line)
    if meta.get('complete') != '1' or int(meta.get('dropped', '-1')) or int(meta.get('inflight', '-1')):
        raise ValueError('Incomplete capture: do not interpret dropped or unfinished reads as fewer waits')
    with Path(str(prefix) + '-reads.csv').open() as f:
        reads = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]
    with Path(str(prefix) + '-frames.csv').open() as f:
        bounds = [{k: int(v) for k, v in row.items()} for row in csv.DictReader(f)]
    assert len(reads) == int(meta['count']) == int(meta['attempted'])
    assert [r['id'] for r in reads] == list(range(len(reads)))
    assert len(bounds) == int(meta['frames']) + 1
    for a, b in zip(bounds, bounds[1:]):
        assert b['frame'] == a['frame'] + 1 and b['boundary_ns'] > a['boundary_ns']
    times = [b['boundary_ns'] for b in bounds]
    nframes = len(times) - 1
    starts = [[] for _ in range(nframes)]
    completes = [0] * nframes
    active = [0] * nframes
    call_overlap = [0] * nframes
    wait_overlap = [0] * nframes
    groups = collections.defaultdict(list)
    hot = collections.defaultdict(list)
    qualified = []
    outside = 0
    for r in reads:
        begin, end = r['request_ns'], r['cpu_return_ns']
        assert 0 < begin <= end
        wb, we = r['wait_begin_ns'], r['wait_end_ns']
        if we:
            assert begin <= wb <= we <= end
        stage = [r[k] for k in ('before_lock_ns', 'enqueued_ns', 'front_popped_ns', 'backend_start_ns', 'area_end_ns', 'flush_end_ns')]
        present = [v for v in stage if v]
        assert present == sorted(present) and (not present or begin <= present[0] <= present[-1] <= end)
        r['duration_ns'] = end - begin
        r['park_ns'] = we - wb if we else 0
        r['queue_ns'] = max(0, r['backend_start_ns'] - r['enqueued_ns']) if r['backend_start_ns'] else 0
        r['front_queue_ns'] = max(0, r['front_popped_ns'] - r['enqueued_ns']) if r['front_popped_ns'] else 0
        r['backend_queue_ns'] = max(0, r['backend_start_ns'] - r['front_popped_ns']) if r['backend_start_ns'] and r['front_popped_ns'] else 0
        if r['backend_start_ns'] and r['front_popped_ns']:
            assert r['queue_ns'] == r['front_queue_ns'] + r['backend_queue_ns']
        r['service_ns'] = max(0, r['flush_end_ns'] - r['backend_start_ns'])
        r['resume_ns'] = max(0, end - r['flush_end_ns']) if r['flush_end_ns'] else 0
        # Stronger candidate filter; still requires a real prequeue dependency proof before a bypass.
        candidate = (not r['split'] and r['fence'] > 0 and r['dirty_mask'] == 0 and
                     not r['pending_channels'] and r['completed_fence_at_request'] >= r['fence'] - 1 and
                     r['requested_generation'] != 0 and
                     len({r[k] for k in ('requested_generation', 'backend_generation', 'checked_generation', 'return_generation')}) == 1 and
                     r['sequence'] == r['checked_sequence'] and r['completed_generation'] != 0)
        if candidate:
            qualified.append(r['id'])
        i = bisect.bisect_right(times, begin) - 1
        if 0 <= i < nframes:
            starts[i].append(r)
        else:
            outside += 1
        j = bisect.bisect_right(times, end) - 1
        if 0 <= j < nframes:
            completes[j] += 1
        for k in range(max(0, i), min(nframes, bisect.bisect_left(times, end))):
            overlap = max(0, min(end, times[k+1]) - max(begin, times[k]))
            if overlap:
                active[k] += 1
                call_overlap[k] += overlap
            if we:
                wait_overlap[k] += max(0, min(we, times[k+1]) - max(wb, times[k]))
        groups[(r['caller'], r['original_size'], r['size'], r['dirty_mask'])].append(r)
        hot[(r['core'], r['caller'], r['original_address'], r['address'] >> 12)].append(r)
    frames = []
    for i, values in enumerate(starts):
        durations = [r['duration_ns'] for r in values]
        frames.append(dict(frame=bounds[i+1]['frame'], start_ns=times[i], end_ns=times[i+1],
                           frame_ms=(times[i+1]-times[i])/1e6, read_starts=len(values),
                           read_completions=completes[i], overlapping_reads=active[i],
                           call_overlap_cpu_us=call_overlap[i]/1e3, parked_overlap_cpu_us=wait_overlap[i]/1e3,
                           started_total_us=sum(durations)/1e3, p50_call_us=percentile(durations, .5)/1e3,
                           p95_call_us=percentile(durations, .95)/1e3, max_call_us=max(durations, default=0)/1e3,
                           queue_total_us=sum(r['queue_ns'] for r in values)/1e3,
                           front_queue_total_us=sum(r['front_queue_ns'] for r in values)/1e3,
                           backend_queue_total_us=sum(r['backend_queue_ns'] for r in values)/1e3,
                           service_total_us=sum(r['service_ns'] for r in values)/1e3,
                           resume_total_us=sum(r['resume_ns'] for r in values)/1e3,
                           clean_at_service=sum(r['dirty_mask'] == 0 for r in values),
                           buffer_gpu_copy_bytes=sum(r.get('buffer_gpu_copy_bytes',0) for r in values),
                           buffer_cpu_copy_bytes=sum(r.get('buffer_cpu_copy_bytes',0) for r in values),
                           gpu_wait_total_us=sum(r.get('gpu_wait_ns',0) for r in values)/1e3,
                           gpu_wait_count=sum(r.get('gpu_wait_count',0) for r in values),
                           all_buffer_gpu_copy_bytes=bounds[i+1].get('buffer_gpu_bytes',0)-bounds[i].get('buffer_gpu_bytes',0),
                           all_buffer_cpu_copy_bytes=bounds[i+1].get('buffer_cpu_bytes',0)-bounds[i].get('buffer_cpu_bytes',0)))
    for i, frame in enumerate(frames):
        for field in ('mapped_direct_bytes','mapped_checked_bytes','mapped_failures'):
            frame[field] = bounds[i+1].get(field,0)-bounds[i].get(field,0)
            assert frame[field] >= 0
    types = []
    for (caller, original_size, size, dirty), values in groups.items():
        d = [v['duration_ns'] for v in values]
        types.append(dict(caller=CALLERS.get(caller, str(caller)), original_bytes=original_size,
                          ordered_bytes=size, dirty_mask=dirty, count=len(values), total_us=sum(d)/1e3,
                          p50_us=percentile(d, .5)/1e3, p95_us=percentile(d, .95)/1e3,
                          max_us=max(d)/1e3, queue_us=sum(v['queue_ns'] for v in values)/1e3,
                          front_queue_us=sum(v['front_queue_ns'] for v in values)/1e3,
                          backend_queue_us=sum(v['backend_queue_ns'] for v in values)/1e3,
                          service_us=sum(v['service_ns'] for v in values)/1e3,
                          resume_us=sum(v['resume_ns'] for v in values)/1e3))
    types.sort(key=lambda r: r['total_us'], reverse=True)
    hotspots = [dict(core=c, caller=CALLERS.get(t,str(t)), original_address=hex(v), device_page=hex(p<<12),
                     count=len(rows), total_us=sum(r['duration_ns'] for r in rows)/1e3)
                for (c,t,v,p), rows in hot.items()]
    hotspots.sort(key=lambda r:r['total_us'], reverse=True)
    repeated = []
    ordered = sorted(reads, key=lambda r:r['fence'])
    for a,b in zip(ordered,ordered[1:]):
        if (a['fence']+1==b['fence'] and a['sequence']==b['sequence'] and
            a['address']//4096==b['address']//4096 and b['enqueued_ns']<=a['flush_end_ns']):
            repeated.append([a['id'],b['id']])
    result = dict(frames=nframes, records=len(reads), dropped=0, starts_outside_timestamp_window=outside,
                  seconds=(times[-1]-times[0])/1e9, fps=nframes*1e9/(times[-1]-times[0]),
                  waits_per_frame=len(reads)/nframes,
                  front_queue_cpu_ms=sum(r['front_queue_ns'] for r in reads)/1e6,
                  backend_queue_cpu_ms=sum(r['backend_queue_ns'] for r in reads)/1e6,
                  total_call_cpu_ms=sum(r['duration_ns'] for r in reads)/1e6,
                  total_parked_cpu_ms=sum(r['park_ns'] for r in reads)/1e6,
                  clean_at_service=sum(r['dirty_mask']==0 for r in reads),
                  all_buffer_copy_metrics_available='buffer_gpu_bytes' in bounds[0],
                  all_buffer_gpu_copy_bytes=bounds[-1].get('buffer_gpu_bytes',0)-bounds[0].get('buffer_gpu_bytes',0),
                  all_buffer_cpu_copy_bytes=bounds[-1].get('buffer_cpu_bytes',0)-bounds[0].get('buffer_cpu_bytes',0),
                  buffer_copy_metrics_available=bool(reads) and 'buffer_gpu_copy_bytes' in reads[0],
                  buffer_gpu_copy_bytes=sum(r.get('buffer_gpu_copy_bytes',0) for r in reads),
                  buffer_cpu_copy_bytes=sum(r.get('buffer_cpu_copy_bytes',0) for r in reads),
                  gpu_wait_cpu_ms=sum(r.get('gpu_wait_ns',0) for r in reads)/1e6,
                  prequeue_check_candidate_ids=qualified,
                  adjacent_same_page_batch_candidate_pairs=repeated,
                  caller_types=types,
                  caveats=['CPU overlap totals sum multiple threads and can exceed frame wall time.',
                           'A zero backend dirty mask does not prove the data was ready when the CPU requested it.',
                           'Candidate filters identify reads to investigate; no coherence ordering has been bypassed.',
                           'Timing includes the bounded trace overhead; compare the same scene with the same instrumentation.',
                           'Only reads starting inside the armed window are recorded; an earlier in-flight read can overlap its first frame.',
                           'Completion-wait time measures the Wait call, including its synchronization overhead, not exclusively time asleep.'])
    result['mapped_metrics_available'] = 'mapped_direct_bytes' in bounds[0]
    for field in ('mapped_direct_bytes','mapped_checked_bytes','mapped_failures'):
        result[field] = sum(frame[field] for frame in frames)
    copy_sources_complete = meta.get('copy_complete') == '1' and int(meta.get('copy_dropped', '-1')) == 0
    result['copy_sources_complete'] = copy_sources_complete if meta.get('copy_sources') == '1' else None
    result['copy_source_records_dropped'] = int(meta.get('copy_dropped', '0'))
    if meta.get('copy_sources') == '1' and not copy_sources_complete and not allow_incomplete_copy_sources:
        raise ValueError('Incomplete buffer-copy source capture')
    # A separate bounded source stream may overflow while every ordered read and
    # frame-boundary aggregate is complete. Explicit opt-in omits source attribution;
    # it never counts missing source records as zero copies or fewer waits.
    if meta.get('copy_sources') == '1' and copy_sources_complete:
        with Path(str(prefix)+'-copies.csv').open() as f:
            copies = [{k:int(v) for k,v in row.items()} for row in csv.DictReader(f)]
        assert len(copies) == int(meta['copy_count']) == int(meta['copy_attempted'])
        sources = collections.defaultdict(list)
        for copy in copies:
            assert 0 < copy['size'] <= copy['buffer_size']
            assert 0 <= copy['offset'] <= copy['buffer_size']-copy['size']
            producer=copy.get('candidate_producer_tick',0)
            assert 0 <= producer <= copy['copy_tick'], 'Producer tick follows readback copy'
            completed=copy.get('candidate_producer_completed',0)
            assert completed in (0,1) and (not completed or producer>0)
            if producer:
                assert copy.get('allocation_id',0)>0 and copy.get('write_version',0)>0
            key=tuple(copy[k] for k in ('buffer_base','buffer_size','sparse_compatible','memory_type')) + (copy.get('allocation_id',0),)
            sources[key].append(copy)
            i=bisect.bisect_right(times,copy['ns'])-1
            if 0<=i<nframes:
                frames[i]['source_copy_count']=frames[i].get('source_copy_count',0)+1
                frames[i]['source_copy_bytes']=frames[i].get('source_copy_bytes',0)+copy['size']
        source_rows=[]
        for (base,size,sparse,mem_type,allocation_id),events in sources.items():
            intervals=sorted((e['offset'],e['offset']+e['size']) for e in events)
            unique=0;last_end=0
            for lo,hi in intervals:
                unique+=max(0,hi-max(lo,last_end));last_end=max(last_end,hi)
            source_rows.append(dict(buffer_base=hex(base),buffer_size=size,allocation_id=allocation_id,
                candidate_producer_copies=sum(bool(e.get('candidate_producer_tick')) for e in events),
                candidate_producer_bytes=sum(e['size'] for e in events if e.get('candidate_producer_tick')),
                candidate_completed_bytes=sum(e['size'] for e in events if e.get('candidate_producer_completed')),
                sparse_compatible=sparse,memory_type=mem_type,copies=len(events),
                copied_bytes=sum(e['size'] for e in events),unique_range_bytes=unique,
                frames=len({e['frame']for e in events}),copy_ticks=len({e['copy_tick']for e in events}),
                async_copies=sum(bool(e['asynchronous'])for e in events)))
        source_rows.sort(key=lambda row:row['copied_bytes'],reverse=True)
        for frame in frames:
            frame.setdefault('source_copy_count',0);frame.setdefault('source_copy_bytes',0)
        result['copy_sources']=dict(events=len(copies),allocations_or_ranges=len(sources),
            copied_bytes=sum(e['size']for e in copies),
            non_sparse_compatible_bytes=sum(e['size']for e in copies if not e['sparse_compatible']),
            async_bytes=sum(e['size']for e in copies if e['asynchronous']),
            candidate_producer_bytes=sum(e['size']for e in copies if e.get('candidate_producer_tick')),
            producer_completion_available=bool(copies) and all('candidate_producer_completed'in e for e in copies),
            candidate_completed_bytes=sum(e['size']for e in copies if e.get('candidate_producer_completed')),
            candidate_earlier_submission_bytes=sum(e['size']for e in copies
                if 0 < e.get('candidate_producer_tick',0) < e['copy_tick']),
            caveat='copy_tick identifies the staging-copy submission, not necessarily the original writer; allocation_id=0 lacks lifetime identity; candidate producer coverage is diagnostic and cannot authorize wait bypass')
        write_csv(Path(str(prefix)+'-copy-sources.csv'),source_rows)
    scalar = [r for r in reads if r.get('scalar_valid')]
    same = [r for r in scalar if r['value_before'] == r['value_after']]
    result['scalar_snapshots'] = dict(count=len(scalar), same_value=len(same),
        same_value_call_cpu_ms=sum(r['duration_ns'] for r in same)/1e6,
        changed_value=len(scalar)-len(same),
        pending_at_request=sum(bool(r['pending_before_valid']) for r in scalar),
        caveat='Equal values do not establish absence of intermediate writes or permission to bypass ordering.')
    write_csv(Path(str(prefix)+'-waits.csv'), [
        dict(id=r['id'], start_frame=r['start_frame'], end_frame=r['end_frame'],
             core=r['core'], caller=CALLERS.get(r['caller'], str(r['caller'])),
             original_address=hex(r['original_address']), original_bytes=r['original_size'],
             ordered_address=hex(r['address']), ordered_bytes=r['size'],
             duration_us=r['duration_ns']/1e3, completion_wait_us=r['park_ns']/1e3,
             queue_us=r['queue_ns']/1e3,
             front_queue_us=r['front_queue_ns']/1e3, backend_queue_us=r['backend_queue_ns']/1e3,
             service_us=r['service_ns']/1e3,
             resume_us=r['resume_ns']/1e3, dirty_mask=r['dirty_mask'],
             buffer_gpu_copy_bytes=r.get('buffer_gpu_copy_bytes',0),
             buffer_cpu_copy_bytes=r.get('buffer_cpu_copy_bytes',0),
             gpu_wait_us=r.get('gpu_wait_ns',0)/1e3,
             gpu_wait_count=r.get('gpu_wait_count',0),
             gpu_wait_tick_max=r.get('gpu_wait_tick_max',0),
             **{k:r.get(k,0) for k in ('scalar_valid','value_before','value_after',
                'pending_before_valid','pending_before','pending_backend_valid','pending_backend')}) for r in reads])
    write_csv(Path(str(prefix)+'-summary.csv'), frames)
    write_csv(Path(str(prefix)+'-types.csv'), types)
    write_csv(Path(str(prefix)+'-hotspots.csv'), hotspots)
    Path(str(prefix)+'-analysis.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('prefix',type=Path,help='Path before -reads.csv/-frames.csv/-meta.txt')
    ap.add_argument('--allow-incomplete-copy-sources', action='store_true',
                    help='Keep complete ordered-read metrics but omit incomplete per-source attribution')
    args=ap.parse_args();r=analyze(args.prefix, args.allow_incomplete_copy_sources)
    print(json.dumps({k:r[k] for k in ('frames','records','dropped','seconds','fps','waits_per_frame','total_call_cpu_ms','total_parked_cpu_ms','clean_at_service')},indent=2))
    print('Wrote per-frame summary, caller types, hot ranges and analysis alongside the raw files.')

if __name__=='__main__': main()
