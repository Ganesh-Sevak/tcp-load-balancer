import React, { useMemo, useState } from 'react';
import { formatBytes, formatDuration, formatRate } from './format.js';
import { ADMIN_BASE, useStats } from './useStats.js';

const statusLabel = {
  live: 'Live',
  mock: 'Mock',
  offline: 'Offline',
  connecting: 'Connecting'
};

function Panel({ title, meta, children, className = '' }) {
  return (
    <section className={`panel ${className}`}>
      <div className="panel-head">
        <h2>{title}</h2>
        {meta && <span>{meta}</span>}
      </div>
      {children}
    </section>
  );
}

function LineChart({ data, field, color, fill = false }) {
  const points = useMemo(() => {
    const max = Math.max(1, ...data.map((item) => item[field] || 0));
    return data
      .map((item, index) => {
        const x = data.length <= 1 ? 0 : (index / (data.length - 1)) * 100;
        const y = 46 - ((item[field] || 0) / max) * 40;
        return `${x.toFixed(2)},${y.toFixed(2)}`;
      })
      .join(' ');
  }, [data, field]);

  const area = points ? `0,48 ${points} 100,48` : '';

  return (
    <div className="chart">
      <svg viewBox="0 0 100 48" preserveAspectRatio="none" role="img">
        {fill && <polygon points={area} fill={color} opacity="0.14" />}
        <polyline points={points} fill="none" stroke={color} strokeWidth="2.2" vectorEffect="non-scaling-stroke" />
      </svg>
    </div>
  );
}

function Gauge({ active, peak }) {
  const percent = Math.min(100, Math.round((active / Math.max(1, peak)) * 100));
  return (
    <div className="gauge">
      <div className="gauge-value">
        <strong>{active.toLocaleString()}</strong>
        <span>{percent}% of observed peak</span>
      </div>
      <div className="gauge-track">
        <div className="gauge-fill" style={{ width: `${percent}%` }} />
      </div>
      <div className="gauge-labels">
        <span>Active</span>
        <span>{peak.toLocaleString()} peak</span>
      </div>
    </div>
  );
}

function LatencyHistogram({ latency }) {
  const buckets = latency?.buckets || [];
  const max = Math.max(1, ...buckets.map((bucket) => bucket.count || 0));
  return (
    <div className="histogram">
      {buckets.slice(0, 10).map((bucket) => (
        <div className="histogram-bar" key={bucket.le_us}>
          <span style={{ height: `${Math.max(6, ((bucket.count || 0) / max) * 100)}%` }} />
          <small>{bucket.le_us >= 1000 ? `${bucket.le_us / 1000}ms` : `${bucket.le_us}us`}</small>
        </div>
      ))}
    </div>
  );
}

function Topology({ stats }) {
  const workers = stats.workers || [];
  const backends = stats.backends || [];
  return (
    <Panel title="Topology" meta={`${workers.length || stats.worker_count} shards`} className="wide topology-panel">
      <div className="topology">
        <div className="topology-node ingress">
          <span>Clients</span>
          <strong>{stats.active_connections.toLocaleString()}</strong>
        </div>
        <div className="worker-bank">
          {workers.slice(0, 12).map((worker) => (
            <div className="worker-node" key={worker.id}>
              <span>w{worker.id}</span>
              <strong>{worker.active_connections.toLocaleString()}</strong>
            </div>
          ))}
        </div>
        <div className="backend-bank">
          {backends.map((backend) => (
            <div className={`topology-node backend ${backend.state.toLowerCase()}`} key={backend.id}>
              <span>{backend.address}</span>
              <strong>{backend.state}</strong>
            </div>
          ))}
        </div>
      </div>
    </Panel>
  );
}

function HealthTimeline({ backend }) {
  const bars = Array.from({ length: 18 }, (_, index) => {
    if (backend.state === 'DOWN' && index > 12) return 'down';
    if (backend.state === 'DRAINING' && index > 9) return 'draining';
    return index % 7 === 0 && backend.errors > 0 ? 'warn' : 'up';
  });
  return (
    <div className="health-timeline">
      {bars.map((state, index) => (
        <span className={state} key={`${backend.id}-${index}`} />
      ))}
    </div>
  );
}

function BackendTable({ backends, onAction, pending, onSelect }) {
  return (
    <Panel title="Backend Pool" meta={`${backends.length} targets`} className="wide">
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
              <th>Timeline</th>
              <th>Controls</th>
            </tr>
          </thead>
          <tbody>
            {backends.map((item) => (
              <tr key={item.id}>
                <td>
                  <button className="link-button mono" onClick={() => onSelect(item)}>
                    {item.address}
                  </button>
                </td>
                <td><span className={`badge ${item.state.toLowerCase()}`}>{item.state}</span></td>
                <td>{item.active_connections.toLocaleString()}</td>
                <td>{item.total_connections.toLocaleString()}</td>
                <td>{formatBytes((item.bytes_in || 0) + (item.bytes_out || 0))}</td>
                <td>{item.errors.toLocaleString()}</td>
                <td>{((item.connect_latency_us || 0) / 1000).toFixed(2)} ms</td>
                <td><HealthTimeline backend={item} /></td>
                <td>
                  <div className="actions">
                    <button disabled={pending === `${item.id}:drain`} onClick={() => onAction(item, 'drain')}>
                      {pending === `${item.id}:drain` ? 'Draining' : 'Drain'}
                    </button>
                    <button disabled={pending === `${item.id}:enable`} onClick={() => onAction(item, 'enable')}>
                      {pending === `${item.id}:enable` ? 'Enabling' : 'Enable'}
                    </button>
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </Panel>
  );
}

function BackendDrawer({ backend, onClose }) {
  if (!backend) return null;
  return (
    <aside className="drawer">
      <button className="drawer-close" onClick={onClose}>Close</button>
      <div>
        <p className="eyebrow">Backend detail</p>
        <h2>{backend.address}</h2>
      </div>
      <span className={`badge ${backend.state.toLowerCase()}`}>{backend.state}</span>
      <div className="drawer-grid">
        <div><span>Active</span><strong>{backend.active_connections.toLocaleString()}</strong></div>
        <div><span>Total</span><strong>{backend.total_connections.toLocaleString()}</strong></div>
        <div><span>Errors</span><strong>{backend.errors.toLocaleString()}</strong></div>
        <div><span>Connect</span><strong>{((backend.connect_latency_us || 0) / 1000).toFixed(2)} ms</strong></div>
      </div>
      <HealthTimeline backend={backend} />
      <div className="metric-row"><span>Ingress bytes</span><strong>{formatBytes(backend.bytes_in || 0)}</strong></div>
      <div className="metric-row"><span>Egress bytes</span><strong>{formatBytes(backend.bytes_out || 0)}</strong></div>
    </aside>
  );
}

export default function App() {
  const { mode, mockEnabled, setMockEnabled, stats, setStats, series } = useStats();
  const [pending, setPending] = useState('');
  const [notice, setNotice] = useState('');
  const [selectedBackend, setSelectedBackend] = useState(null);
  const latest = series.at(-1) || { connRate: 0, bytesRate: 0, p99: 0 };

  const applyBackendAction = async (backend, action) => {
    const targetState = action === 'drain' ? 'DRAINING' : 'UP';
    if (!window.confirm(`${action === 'drain' ? 'Drain' : 'Enable'} ${backend.address}?`)) return;

    const previousBackends = stats.backends || [];
    setPending(`${backend.id}:${action}`);
    setNotice('');
    setStats((current) => ({
      ...current,
      backends: (current.backends || []).map((item) => (item.id === backend.id ? { ...item, state: targetState } : item))
    }));

    try {
      if (!mockEnabled) {
        const response = await fetch(`${ADMIN_BASE}/backends/${backend.id}/${action}`, { method: 'POST' });
        if (!response.ok) throw new Error(`admin returned ${response.status}`);
        setStats(await response.json());
      }
      setNotice(`${backend.address} ${targetState.toLowerCase()}`);
    } catch {
      setStats((current) => ({ ...current, backends: previousBackends }));
      setNotice(`Command failed for ${backend.address}`);
    } finally {
      setPending('');
    }
  };

  return (
    <main>
      <header className="topbar">
        <div>
          <p className="eyebrow">Sharded TCP load balancer</p>
          <h1>{stats.listener}</h1>
        </div>
        <div className="status-strip">
          <span className={`live-dot ${mode}`} />
          <span>{statusLabel[mode] || mode}</span>
          <span>{stats.policy}</span>
          <span>{stats.worker_count} workers</span>
          <span>{formatDuration(stats.uptime_seconds)}</span>
          <button className={mockEnabled ? 'toggle active' : 'toggle'} onClick={() => setMockEnabled((value) => !value)}>
            Mock {mockEnabled ? 'On' : 'Off'}
          </button>
        </div>
      </header>

      {notice && <div className="notice">{notice}</div>}

      <section className="grid">
        <Panel title="Throughput" meta={formatRate(latest.connRate)}>
          <LineChart data={series} field="connRate" color="#2dd4bf" fill />
          <div className="split-metrics">
            <div><span>Bytes in</span><strong>{formatBytes(stats.bytes_in)}</strong></div>
            <div><span>Bytes out</span><strong>{formatBytes(stats.bytes_out)}</strong></div>
            <div><span>Wire rate</span><strong>{formatBytes(latest.bytesRate)}</strong></div>
          </div>
        </Panel>

        <Panel title="Connections" meta={`${stats.total_connections.toLocaleString()} total`}>
          <Gauge active={stats.active_connections} peak={stats.peak_connections} />
          <LineChart data={series} field="active" color="#60a5fa" />
        </Panel>

        <Panel title="Connect Latency" meta={`p99 ${Number(stats.latency?.p99 || latest.p99).toFixed(2)} ms`}>
          <div className="latency-grid">
            <div><span>p50</span><strong>{Number(stats.latency?.p50 || 0).toFixed(2)} ms</strong></div>
            <div><span>p99</span><strong>{Number(stats.latency?.p99 || 0).toFixed(2)} ms</strong></div>
            <div><span>p99.9</span><strong>{Number(stats.latency?.p999 || 0).toFixed(2)} ms</strong></div>
          </div>
          <LatencyHistogram latency={stats.latency} />
        </Panel>

        <Panel title="Reliability" meta={`${stats.errors.toLocaleString()} errors`}>
          <div className="metric-row"><span>Connect timeouts</span><strong>{stats.connect_timeouts}</strong></div>
          <div className="metric-row"><span>Idle reaps</span><strong>{stats.idle_timeouts}</strong></div>
          <div className="metric-row"><span>Admin</span><strong>{ADMIN_BASE.replace('http://', '')}</strong></div>
          <LineChart data={series} field="errors" color="#f87171" />
        </Panel>

        <Topology stats={stats} />
        <BackendTable
          backends={stats.backends || []}
          onAction={applyBackendAction}
          pending={pending}
          onSelect={setSelectedBackend}
        />
      </section>

      <BackendDrawer backend={selectedBackend} onClose={() => setSelectedBackend(null)} />
    </main>
  );
}
