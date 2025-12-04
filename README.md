# LwM2M Send Scheduler Demo

This project is a Zephyr-based sample that augments the base LwM2M client with
the upstream send scheduler helper. It demonstrates how to control time-series
resource caching and SEND behaviour using the Scheduler Control (10523) and
Sampling Rules (10524) LwM2M objects now provided by Zephyr.

## Building & Running

Prerequisites:

- Zephyr SDK / toolchain and west environment initialised.
- `west` workspace with this application checked out (as in the default
  sandbox).

Build for the native simulator:

```bash
west build -b native_sim .
```

Run the native simulator binary:

```bash
west build -t run           # build + run
# or
./build/zephyr/zephyr.exe   # rerun the last build
```

## Configuring Scheduler Rules

The scheduler uses object 10524 instances to control cache behaviour for
specific resource paths. Each instance requires:

- **Path string** (resource path like `/3303/0/5700`)
- One or more rule entries (multi-instance resource) using the syntax:
  - `gt=VALUE` / `lt=VALUE` – Edge-triggered upper/lower thresholds
  - `st=VALUE` – Minimum step between cached samples
  - `pmin=SECONDS` – Minimum interval between accepted samples
  - `pmax=SECONDS` – Maximum interval; when it expires the last value is
    re-cached

Rules are combined with logical OR semantics:

- Any threshold crossing (`gt` or `lt`) immediately allows the sample.
- `st` requires either the first sample or a delta ≥ threshold.
- `pmin` defers caching until the timer expires even if other rules match.
- `pmax` will clone the last sample into the cache once the deadline passes to
  ensure a value exists for the next SEND.

## Working With DDF

To experiment with the rules in a UI such as Eclipse Leshan:

1. Import the DDF files from `ddf/send_scheduler.xml`. You can use the `-m`
command argument to specify a path to load extra model for the Leshan demo
server.
2. Provision the `/10524/*` instances to point at `/3303/0/5700` or any other
   resource you enable caching for in the application.
3. Adjust rules live to observe how the scheduler throttles or injects samples.

## License

This sample inherits Zephyr’s license unless otherwise noted. Refer to the
Zephyr project licensing terms for details.
