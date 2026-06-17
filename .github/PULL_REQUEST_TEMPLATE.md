## Summary

- 

## Type

- [ ] Data path / server
- [ ] Scheduler / health
- [ ] Admin API / observability
- [ ] Dashboard
- [ ] Benchmark / docs
- [ ] CI / quality

## Verification

- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- [ ] `cmake --build build --parallel`
- [ ] `ctest --test-dir build --output-on-failure`
- [ ] `npm --prefix web run build`
- [ ] Linux/Docker validation, if server code changed
- [ ] Screenshot attached, if dashboard changed

## Notes

- Performance claims added? If yes, link benchmark metadata and raw command output.
- Public API changed? If yes, document `/stats`, `/metrics`, config, or CLI changes.
