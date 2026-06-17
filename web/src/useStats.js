import { useEffect, useRef, useState } from 'react';
import { makeMockStats } from './mockStats.js';

export const ADMIN_BASE = import.meta.env.VITE_ADMIN_BASE || 'http://127.0.0.1:9100';
const WINDOW_SIZE = 64;

const initialStats = makeMockStats(16);
const initialSeries = Array.from({ length: 28 }, (_, index) => {
  const previous = makeMockStats(index);
  const next = makeMockStats(index + 1);
  const previousBytes = previous.bytes_in + previous.bytes_out;
  const nextBytes = next.bytes_in + next.bytes_out;
  return {
    connRate: next.connections_per_sec,
    bytesRate: Math.max(0, nextBytes - previousBytes),
    active: next.active_connections,
    p99: next.latency?.p99 || 0,
    errors: next.errors || 0
  };
});

export function useStats() {
  const [mode, setMode] = useState('mock');
  const [mockEnabled, setMockEnabled] = useState(true);
  const [stats, setStats] = useState(() => initialStats);
  const [series, setSeries] = useState(() => initialSeries);
  const previous = useRef({
    total: initialStats.total_connections,
    bytes: initialStats.bytes_in + initialStats.bytes_out
  });

  const pushStats = (next, nextMode) => {
    setMode(nextMode);
    setStats(next);
    setSeries((current) => {
      const last = previous.current;
      const bytes = (next.bytes_in || 0) + (next.bytes_out || 0);
      const bytesRate = last ? Math.max(0, bytes - last.bytes) : 0;
      const connRate = Math.round(next.connections_per_sec || (last ? Math.max(0, next.total_connections - last.total) : 0));
      previous.current = { total: next.total_connections || 0, bytes };
      return [
        ...current,
        {
          connRate,
          bytesRate,
          active: next.active_connections || 0,
          p99: next.latency?.p99 || 0,
          errors: next.errors || 0
        }
      ].slice(-WINDOW_SIZE);
    });
  };

  useEffect(() => {
    let tick = 0;
    let mockTimer;
    let eventSource;
    let pollTimer;
    let cancelled = false;

    const startMock = () => {
      clearInterval(mockTimer);
      mockTimer = setInterval(() => {
        tick += 1;
        pushStats(makeMockStats(tick), 'mock');
      }, 450);
    };

    if (mockEnabled) {
      startMock();
      return () => clearInterval(mockTimer);
    }

    const pollStats = async () => {
      try {
        const response = await fetch(`${ADMIN_BASE}/stats`);
        if (!response.ok) throw new Error(`stats returned ${response.status}`);
        if (!cancelled) pushStats(await response.json(), 'live');
      } catch {
        if (!cancelled) setMode('offline');
      }
    };

    try {
      eventSource = new EventSource(`${ADMIN_BASE}/events`);
      const onStats = (event) => pushStats(JSON.parse(event.data), 'live');
      eventSource.onmessage = onStats;
      eventSource.addEventListener('stats', onStats);
      eventSource.onerror = () => {
        eventSource.close();
        setMode('offline');
      };
    } catch {
      setMode('offline');
    }

    pollStats();
    pollTimer = setInterval(pollStats, 2000);

    return () => {
      cancelled = true;
      clearInterval(pollTimer);
      eventSource?.close();
    };
  }, [mockEnabled]);

  return { mode, mockEnabled, setMockEnabled, stats, setStats, series };
}
