export const formatBytes = (value = 0) => {
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  if (value < 1024 * 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MiB`;
  return `${(value / 1024 / 1024 / 1024).toFixed(1)} GiB`;
};

export const formatDuration = (seconds = 0) => {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  return h > 0 ? `${h}h ${m}m ${s}s` : `${m}m ${s}s`;
};

export const formatRate = (value = 0) => {
  if (value >= 1000000) return `${(value / 1000000).toFixed(1)}M/s`;
  if (value >= 1000) return `${(value / 1000).toFixed(1)}K/s`;
  return `${Math.round(value)}/s`;
};
