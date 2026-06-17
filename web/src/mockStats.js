const backend = (id, address, state, active, total, bytesIn, bytesOut, errors, connectLatencyUs, trend) => ({
  id,
  address,
  state,
  active_connections: Math.round(active),
  total_connections: Math.round(total),
  bytes_in: Math.round(bytesIn),
  bytes_out: Math.round(bytesOut),
  errors,
  connect_latency_us: connectLatencyUs,
  trend
});

export const makeMockStats = (tick = 0) => {
  const wave = Math.sin(tick / 4) + 1;
  const burst = Math.sin(tick / 11) + 1;
  const active = Math.floor(520 + wave * 210 + (tick % 9) * 13);
  const total = 18000 + tick * 330;
  const bytesIn = 620000000 + tick * 1400000;
  const bytesOut = 617000000 + tick * 1380000;
  const p50 = 0.6 + wave * 0.18;
  const p99 = 3.8 + burst * 1.45;
  const p999 = 9.4 + wave * 2.4;

  return {
    uptime_seconds: 5400 + tick,
    listener: '0.0.0.0:9000',
    policy: tick % 24 < 12 ? 'least-connections' : 'power-of-two-choices',
    worker_count: 8,
    active_connections: active,
    peak_connections: 12880,
    total_connections: total,
    connections_per_sec: 720 + Math.round(wave * 110),
    bytes_in: bytesIn,
    bytes_out: bytesOut,
    errors: 4,
    connect_timeouts: 1,
    idle_timeouts: 64,
    latency: {
      kind: 'backend_connect',
      p50,
      p99,
      p999,
      count: 1200 + tick * 12,
      sum_us: 980000 + tick * 4200,
      buckets: [
        { le_us: 100, count: 20 },
        { le_us: 250, count: 128 },
        { le_us: 500, count: 430 },
        { le_us: 1000, count: 760 + tick },
        { le_us: 2500, count: 980 + tick * 4 },
        { le_us: 5000, count: 1120 + tick * 8 },
        { le_us: 10000, count: 1190 + tick * 10 },
        { le_us: 25000, count: 1200 + tick * 12 }
      ]
    },
    workers: Array.from({ length: 8 }, (_, id) => ({
      id,
      active_connections: Math.round(active / 8 + Math.sin((tick + id) / 3) * 16),
      total_connections: Math.round(total / 8 + id * 22),
      peak_connections: 1700 + id * 35,
      bytes_in: Math.round(bytesIn / 8),
      bytes_out: Math.round(bytesOut / 8),
      errors: id === 5 ? 1 : 0
    })),
    backends: [
      backend(0, '127.0.0.1:9101', 'UP', active * 0.49, total * 0.48, bytesIn * 0.49, bytesOut * 0.49, 0, 420, 'steady'),
      backend(1, '127.0.0.1:9102', tick % 32 > 26 ? 'DRAINING' : 'UP', active * 0.34, total * 0.34, bytesIn * 0.33, bytesOut * 0.35, 2, 560, 'draining'),
      backend(2, '127.0.0.1:9103', tick % 44 > 38 ? 'DOWN' : 'UP', active * 0.17, total * 0.18, bytesIn * 0.18, bytesOut * 0.16, 2, 890, 'recovering')
    ]
  };
};
