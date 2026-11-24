"""Account native bin spans (elapsed time, NOT thread CPU). Independent of payload size."""
import math


def balance(log):
    def rows(tag):
        return [dict(item.split('=', 1) for item in line.split()[1:])
                for line in log.splitlines() if line.startswith(tag + ' ')]

    def require(test, reason):
        if not test:
            raise ValueError(reason)

    front, = rows('FRONT64')
    teams = int(front['team'])
    total = int(front['m'])
    nonempty = int(front['nonempty_bins'])
    require(0 < teams <= 64 and 0 < nonempty <= min(4096, total), 'invalid histogram')
    summary, lanes = rows('FRONT64_BINS'), rows('FRONT64_LANE')
    require(sorted(s['phase'] for s in summary) == ['order', 'publish'] and len(lanes) == 2*teams,
            'incomplete bin clocks')
    result = {}
    for s in summary:
        phase = s['phase']
        col = sorted((r for r in lanes if r['phase'] == phase), key=lambda r: int(r['lane']))
        require([int(r['lane']) for r in col] == list(range(teams)), 'missing worker')
        bins = [int(r['bins']) for r in col]
        counts = [int(r['rows']) for r in col]
        busy = [float(r['active_s']) for r in col]
        require(sum(bins) == nonempty and sum(counts) == total and all(0 <= b <= c for b, c in zip(bins, counts)),
                'different row ownership')
        wall, active, sort, label, longest = (float(s[k]) for k in ('wall_s', 'active_s', 'sort_s', 'label_s', 'longest_s'))
        eps = 1e-7 * teams
        require(wall > 0 and active > 0 and all(math.isfinite(x) and x >= 0 for x in [wall, active, sort, label, longest, *busy]), 'invalid bin clock')
        require(abs(sum(busy) - active) <= eps and longest <= max(busy) + eps and max(busy) <= wall + eps,
                'impossible bin timeline')
        require(abs(active-sort-label) <= eps if phase == 'order' else sort == label == 0, 'wrong subclocks')
        require(0 <= int(s['longest_bin']) < 4096 and 0 < int(s['longest_rows']) <= int(front['max_bin']), 'invalid slowest bin')
        result[phase] = dict(wall_s=wall, active_lane_s=active, active_fraction=active/(teams*wall),
                             longest_bin=int(s['longest_bin']), longest_bin_rows=int(s['longest_rows']),
                             longest_bin_s=longest, longest_bin_fraction=longest/wall,
                             sort_lane_s=sort, label_lane_s=label, lane_rows=counts, lane_active_s=busy)
    return result
