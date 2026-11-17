# Offline checks (reference)

`vk/check.sh` is the no-phone gate (~40 s with `--no-build`). It builds the ICD for x86, runs the unit and `build-t_*.sh` tests, the vk_* tests on the
fake GPUs v11/v13, and compares decoded command-stream goldens.

- **Goldens**: `vk/tests/golden/<test>-<gpu>.txt`, written from the fake backend dump (`MALI_FAKE_DUMP`). A mismatch prints a short diff.
  If the change is intended: `./check.sh --bless`, review `git diff vk/tests/golden`, commit it with the change that caused it.
- **Rules** (`vk/tests/golden.py`, `src/mali_backend_fake.c`): R-VA, R-GEOM, R-HEAP, R-SET, R-CS (v11 RUN_IDVS and v13 RUN_IDVS2), R-FLAGS.
  Each has a self-test in check.sh (`tests/selftest.py`, `MALI_FAKE_BADFLAGS`, `MALI_GEOM_SIZE`); a rule that cannot be made to fire does not belong there.
- **Sanitizers**: `MALI_SAN=1 [MALI_SAN_LEAKS=1] ./check.sh --no-build` (ASan+UBSan driver in `out/native-san`; build it with
  `MALI_SAN=1 MALI_TARGET=native-san vk/build.sh`).
- **Host dEQP** (opt-in, slow): `./check.sh --deqp-host`, or `vk/deqp/host-run.sh [--bless]`. Needs the host `deqp-vk` in `third_party/build-vk-x86-host`
  (build recipe in `vk/deqp/README.md`); skipped with a message when absent. Gates only Crash/Timeout/NotSupported against `vk/deqp/host-baseline.tsv`.
