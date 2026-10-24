# PID Temperature Testbench

A NUCLEO-G070RB holds a heated block at a setpoint with one of three
controllers, so they can be compared on the same hardware:

- **MANUAL**: fixed baseline gains
- **ZN**: Ziegler–Nichols gains from a relay autotune
- **ADAPTIVE**: gain scheduling on the size of the error

![Control loop](docs/images/block-diagram.svg)

![Wiring](docs/images/wiring.svg)

## Quick start

**Firmware:** open `firmware/pid_testbench.ioc` in STM32CubeIDE and build, or:

```
cd firmware
make && make flash
```

**Dashboard:**

```
cd dashboard
pip install -r requirements.txt
python dashboard.py
```

Connect to the ST-LINK port, pick a mode, set the setpoint, press *Start*.

![Dashboard](docs/images/dashboard.png)

Pin table, serial commands, telemetry format, safety limits and metric
definitions are in [docs/reference.md](docs/reference.md).
