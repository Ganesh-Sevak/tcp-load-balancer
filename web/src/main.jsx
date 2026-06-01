import React, { useEffect, useMemo, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import './styles.css';

const ADMIN_BASE = import.meta.env.VITE_ADMIN_BASE || 'http://127.0.0.1:9100';
const WINDOW_SIZE = 48;

const formatBytes = (value) => {
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  if (value < 1024 * 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MiB`;
  return `${(value / 1024 / 1024 / 1024).toFixed(1)} GiB`;
};

const formatDuration = (seconds) => {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  return h > 0 ? `${h}h ${m}m ${s}s` : `${m}m ${s}s`;
};

const makeMockStats = (tick) => {
  const wave = Math.sin(tick / 4) + 1;
  const active = Math.floor(420 + wave * 170 + (tick % 7) * 11);
  const total = 10000 + tick * 280;
  const bytesIn = 220000000 + tick * 900000;
  const bytesOut = 219000000 + tick * 880000;

  return {
    uptime_seconds: 3700 + tick,
    listener: '0.0.0.0:9000',
    policy: tick % 16 < 8 ? 'least-connections' : 'power-of-two-choices',
    worker_count: 8,
    active_connections: active,
    peak_connections: 11840,
    total_connections: total,
    connections_per_sec: 640 + Math.round(wave * 80),
    bytes_in: bytesIn,
    bytes_out: bytesOut,
    errors: 3,
    connect_timeouts: 1,
    idle_timeouts: 42,
    latency: { p50: 0.9 + wave * 0.2, p99: 4.2 + wave * 1.3, p999: 9.8 + wave * 2.1 },
    backends: [
      backend(0, '127.0.0.1:9101', 'UP', active * 0.52, total * 0.5, bytesIn * 0.51, bytesOut * 0.5, 0, 490),
      backend(1, '127.0.0.1:9102', tick % 30 > 24 ? 'DRAINING' : 'UP', active * 0.31, total * 0.32, bytesIn * 0.3, bytesOut * 0.33, 2, 530),
      backend(2, '127.0.0.1:9103', tick % 40 > 34 ? 'DOWN' : 'UP', active * 0.17, total * 0.18, bytesIn * 0.19, bytesOut * 0.17, 1, 710)
    ]
  };
};

const backend = (id, address, state, active, total, bytesIn, bytesOut, errors, connectLatencyUs) => ({
  id,
  address,
  state,
  active_connections: Math.round(active),
  total_connections: Math.round(total),
  bytes_in: Math.round(bytesIn),
  bytes_out: Math.round(bytesOut),
  errors,
  connect_latency_us: connectLatencyUs
});

function useStats() {
  const [mode, setMode] = useState('connecting');
  const [stats, setStats] = useState(() => makeMockStats(0));
  const [series, setSeries] = useState([]);
  const previous = useRef(null);

  useEffect(() => {
    let tick = 0;
    let mockTimer;
    let eventSource;
    let pollTimer;

    const pushStats = (next, nextMode) => {
      setMode(nextMode);
      setStats(next);
      setSeries((current) => {
        const last = previous.current;
        const bytesRate = last ? Math.max(0, next.bytes_in + next.bytes_out - last.bytes) : 0;
        const connRate = Math.round(next.connections_per_sec || (last ? Math.max(0, next.total_connections - last.total) : 0));
        previous.current = { total: next.total_connections, bytes: next.bytes_in + next.bytes_out };
        return [...current, { connRate, bytesRate, active: next.active_connections, p99: next.latency?.p99 || 0 }].slice(-WINDOW_SIZE);
      });
    };

    const startMock = () => {
      clearInterval(mockTimer);
      mockTimer = setInterval(() => {
        tick += 1;
        pushStats(makeMockStats(tick), 'mock');
      }, 450);
    };

    try {
      eventSource = new EventSource(`${ADMIN_BASE}/events`);
      eventSource.onmessage = (event) => pushStats(JSON.parse(event.data), 'live');
      eventSource.addEventListener('stats', (event) => pushStats(JSON.parse(event.data), 'live'));
      eventSource.onerror = () => {
        eventSource.close();
        startMock();
      };
    } catch {
      startMock();
    }

    pollTimer = setInterval(async () => {
      if (mode === 'live') return;
      try {
        const response = await fetch(`${ADMIN_BASE}/stats`);
        if (response.ok) {
          pushStats(await response.json(), 'live');
        }
      } catch {
        if (!mockTimer) startMock();
      }
    }, 2000);

    return () => {
      clearInterval(mockTimer);
      clearInterval(pollTimer);
      eventSource?.close();
    };
  }, []);

  return { mode, stats, series };
}

function LineChart({ data, field, label, color }) {
  const points = useMemo(() => {
    const max = Math.max(1, ...data.map((item) => item[field]));
    return data
      .map((item, index) => {
        const x = data.length <= 1 ? 0 : (index / (data.length - 1)) * 100;
        const y = 44 - (item[field] / max) * 38;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      })
      .join(' ');
  }, [data, field]);

  return (
    <div className="chart" aria-label={label}>
      <svg viewBox="0 0 100 48" preserveAspectRatio="none">
        <polyline points={points} fill="none" stroke={color} strokeWidth="2.3" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
}

function Gauge({ active, peak }) {
  const percent = Math.min(100, Math.round((active / Math.max(1, peak)) * 100));
  return (
    <div className="gauge">
      <div className="gauge-track">
        <div className="gauge-fill" style={{ width: `${percent}%` }} />
      </div>
      <div className="gauge-labels">
        <span>{active.toLocaleString()} active</span>
        <span>{peak.toLocaleString()} peak</span>
      </div>
    </div>
  );
}

function BackendTable({ backends }) {
  const postAction = async (id, action) => {
    await fetch(`${ADMIN_BASE}/backends/${id}/${action}`, { method: 'POST' });
  };

  return (
    <section className="panel wide">
      <div className="panel-head">
        <h2>Backends</h2>
        <span>{backends.length} targets</span>
      </div>
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>Address</th>
              <th>Health</th>
              <th>Active</th>
              <th>Total</th>
              <th>Bytes</th>
              <th>Errors</th>
              <th>Connect</th>
              <th>Controls</th>
            </tr>
          </thead>
          <tbody>
            {backends.map((item) => (
              <tr key={item.id}>
                <td className="mono">{item.address}</td>
                <td><span className={`badge ${item.state.toLowerCase()}`}>{item.state}</span></td>
                <td>{item.active_connections.toLocaleString()}</td>
                <td>{item.total_connections.toLocaleString()}</td>
                <td>{formatBytes(item.bytes_in + item.bytes_out)}</td>
                <td>{item.errors.toLocaleString()}</td>
                <td>{(item.connect_latency_us / 1000).toFixed(2)} ms</td>
                <td>
                  <div className="actions">
                    <button onClick={() => postAction(item.id, 'drain')}>Drain</button>
                    <button onClick={() => postAction(item.id, 'enable')}>Enable</button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

function App() {
  const { mode, stats, series } = useStats();
  const latest = series.at(-1) || { connRate: 0, bytesRate: 0, p99: 0 };

  return (
    <main>
      <header className="topbar">
        <div>
          <p className="eyebrow">TCP Load Balancer</p>
          <h1>{stats.listener}</h1>
        </div>
        <div className="status-strip">
          <span className={`live-dot ${mode}`} />
          <span>{mode}</span>
          <span>{stats.policy}</span>
          <span>{stats.worker_count} workers</span>
          <span>{formatDuration(stats.uptime_seconds)}</span>
        </div>
      </header>

      <section className="grid">
        <article className="panel">
          <div className="panel-head">
            <h2>Throughput</h2>
            <span>{latest.connRate.toLocaleString()} conn/s</span>
          </div>
          <LineChart data={series} field="connRate" label="connections per second" color="#5eead4" />
          <div className="metric-row">
            <span>Bytes in</span>
            <strong>{formatBytes(stats.bytes_in)}</strong>
          </div>
          <div className="metric-row">
            <span>Bytes out</span>
            <strong>{formatBytes(stats.bytes_out)}</strong>
          </div>
        </article>

        <article className="panel">
          <div className="panel-head">
            <h2>Connections</h2>
            <span>{stats.total_connections.toLocaleString()} total</span>
          </div>
          <Gauge active={stats.active_connections} peak={stats.peak_connections} />
          <LineChart data={series} field="active" label="active connections" color="#a78bfa" />
        </article>

        <article className="panel">
          <div className="panel-head">
            <h2>Latency</h2>
            <span>p99 {Number(stats.latency?.p99 || latest.p99).toFixed(2)} ms</span>
          </div>
          <div className="latency-grid">
            <div><span>p50</span><strong>{Number(stats.latency?.p50 || 0).toFixed(2)} ms</strong></div>
            <div><span>p99</span><strong>{Number(stats.latency?.p99 || 0).toFixed(2)} ms</strong></div>
            <div><span>p99.9</span><strong>{Number(stats.latency?.p999 || 0).toFixed(2)} ms</strong></div>
          </div>
          <LineChart data={series} field="p99" label="p99 latency" color="#f59e0b" />
        </article>

        <article className="panel">
          <div className="panel-head">
            <h2>Reliability</h2>
            <span>{stats.errors.toLocaleString()} errors</span>
          </div>
          <div className="metric-row"><span>Connect timeouts</span><strong>{stats.connect_timeouts}</strong></div>
          <div className="metric-row"><span>Idle reaps</span><strong>{stats.idle_timeouts}</strong></div>
          <div className="metric-row"><span>Admin endpoint</span><strong>{ADMIN_BASE.replace('http://', '')}</strong></div>
        </article>

        <BackendTable backends={stats.backends || []} />
      </section>
    </main>
  );
}

createRoot(document.getElementById('root')).render(<App />);

