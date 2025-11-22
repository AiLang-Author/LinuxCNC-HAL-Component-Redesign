# HAL Microkernel for LinuxCNC

A modern, non-blocking microkernel architecture for LinuxCNC HAL components, written in AILang and compiled to native x86-64 assembly.

## 🎯 Project Goals

Traditional LinuxCNC HAL components suffer from a fundamental architectural problem: they run in the RTAPI (realtime) thread, which means **any blocking operation** (sleep, wait, I/O) can cause timing violations and system instability. This severely limits what HAL components can do.

**HAL Microkernel solves this by:**

1. **Process Isolation** - Services run in separate processes outside RTAPI
2. **Non-blocking Communication** - Shared memory IPC with zero-copy operations
3. **Fault Tolerance** - Automatic service restart on crash
4. **Minimal CPU Usage** - Adaptive sleep (10ms idle, 100µs busy) = ~0.3% CPU
5. **Pin Monitoring** - Real-time HAL pin change detection and callbacks
6. **Standard Interface** - Clean C bridge component for seamless HAL integration

## 🏗️ Architecture

```
┌─────────────────────────────────────┐
│   LinuxCNC RTAPI (realtime)         │
│   ┌─────────────────────────────┐   │
│   │  HAL Bridge Component (C)   │   │
│   │  - Zero blocking            │   │
│   │  - Pure memory operations   │   │
│   └──────────┬──────────────────┘   │
└──────────────┼──────────────────────┘
               │ Shared Memory (/tmp/hal_pins.shm)
               │ (memory-mapped file, non-blocking)
┌──────────────▼──────────────────────┐
│   HAL Microkernel (user space)      │
│   ┌─────────────────────────────┐   │
│   │  Kernel Main Loop           │   │
│   │  - Message Queue            │   │
│   │  - Service Manager          │   │
│   │  - Pin Monitor              │   │
│   │  - Health Checks            │   │
│   └──────────┬──────────────────┘   │
│              │                       │
│   ┌──────────▼──────────────┐       │
│   │  Service 1 (process)    │       │
│   │  - Can block/sleep      │       │
│   │  - Network I/O OK       │       │
│   │  - File operations OK   │       │
│   └─────────────────────────┘       │
│   ┌─────────────────────────┐       │
│   │  Service 2 (process)    │       │
│   │  - Auto-restart on crash│       │
│   └─────────────────────────┘       │
└─────────────────────────────────────┘
```

## 📁 Project Structure

```
hal-microkernel/
├── HAL_Microkernel.ailang          # Main microkernel daemon
├── HAL_Pin_Monitor.ailang          # Standalone pin monitor (demo)
├── HAL_Pin_Poke.ailang             # CLI tool to write pins
├── HAL_Pin_Stress.ailang           # Stress testing tool
├── hal_microkernel_bridge.c        # HAL component bridge
└── README.md                       # This file
```

## 🚀 Quick Start

### 1. Compile the Microkernel

```bash
# Requires AILang compiler
python3 ailang_compiler.py HAL_Microkernel.ailang
python3 ailang_compiler.py HAL_Pin_Poke.ailang
python3 ailang_compiler.py HAL_Pin_Stress.ailang
```

### 2. Start the Daemon

```bash
./HAL_Microkernel_exec
# Prints: Kernel daemonized with PID: 12345
# Creates: /tmp/hal_pins.shm (4KB shared memory file)
```

### 3. Install HAL Bridge Component

```bash
# On LinuxCNC machine
halcompile --install hal_microkernel_bridge.c
```

### 4. Configure in HAL

```bash
# In your .hal file
loadrt hal_microkernel_bridge
addf microkernel.update servo-thread

# Connect pins
net spindle-speed motion.spindle-speed-out => microkernel.pin.0.in
net axis-position microkernel.pin.1.out => stepgen.0.position-cmd
net estop-signal iocontrol.0.user-enable-out => microkernel.pin.3.in
```

## 🔧 Components

### HAL Microkernel (Core Daemon)

**Binary:** `HAL_Microkernel_exec` (30KB)

**Features:**
- Persistent daemon process
- 64 service slots
- 256-message circular queue
- 256 pin slots with change detection
- Automatic service restart (max 3 attempts)
- Adaptive sleep for minimal CPU usage
- File-backed shared memory (`/tmp/hal_pins.shm`)

**Default Pins:**
- Pin 0: `spindle.speed`
- Pin 1: `axis.0.pos-cmd`
- Pin 2: `axis.1.pos-cmd`
- Pin 3: `estop.triggered`

**Control:**
```bash
# Stop daemon
kill -TERM <PID>

# Check if running
ps aux | grep HAL_Microkernel
```

### HAL Bridge Component (C)

**File:** `hal_microkernel_bridge.c`

**Pins Created:**
- `microkernel.pin.0-15.in` (HAL_IN, float) - Write to microkernel
- `microkernel.pin.0-15.out` (HAL_OUT, float) - Read from microkernel
- `microkernel.connected` (HAL_OUT, bit) - Connection status
- `microkernel.update-count` (HAL_OUT, u32) - Total updates
- `microkernel.error-count` (HAL_OUT, u32) - Error counter

**Realtime Thread:** Pure memory operations, zero blocking

### Pin Poker Tool

**Binary:** `HAL_Pin_Poke_exec`

Manual pin testing from command line:

```bash
./HAL_Pin_Poke_exec <pin_id> <value>

# Examples
./HAL_Pin_Poke_exec 0 1500    # Set spindle speed
./HAL_Pin_Poke_exec 3 1       # Trigger estop
```

### Stress Tester

**Binary:** `HAL_Pin_Stress_exec`

Continuous random pin updates for stability testing:

```bash
./HAL_Pin_Stress_exec
# Updates all 4 pins every 100ms with random values
# Prints statistics every 10 seconds
```

## 📊 Performance

- **CPU Usage:** ~0.3-0.7% under load
- **Memory:** 224KB (stable, no leaks)
- **Update Rate:** 400+ pin updates/second
- **Latency:** <100µs (adaptive sleep)
- **Binary Size:** 30KB (microkernel), 21KB (tools)

## 🛠️ Service Development

Services run in isolated processes and can:
- ✅ Use blocking I/O (files, network, serial)
- ✅ Call `sleep()` / `usleep()`
- ✅ Wait on mutexes/semaphores
- ✅ Perform long computations
- ✅ Auto-restart on crash (configurable)

### Service Structure

```c
// Service struct layout (80 bytes per service)
[0]   state              // UNINITIALIZED, READY, RUNNING, etc.
[8]   pid                // Process ID
[16]  handler_ptr        // Service entry point
[24]  stack_ptr          // Service stack
[32]  user_data          // Custom data pointer
[40]  restart_count      // Number of restarts
[48]  (reserved)
[56]  auto_restart_flag  // Enable auto-restart
[64]  last_restart_time  // Timestamp
[72]  total_crashes      // Lifetime crash count
```

## 📡 Shared Memory Layout

**File:** `/tmp/hal_pins.shm` (4096 bytes)

```
Offset   Size    Description
------   ----    -----------
0-7      8       Pin count
8-15     8       Update flag (set by writers)
16-23    8       Pin 0 value (int64_t / float)
24-31    8       Pin 1 value
32-39    8       Pin 2 value
...              (up to 256 pins)
```

**Type Conversion:** Floats stored as int64_t via union (preserves bit pattern)

## 🔍 Monitoring & Debugging

### Check Daemon Status

```bash
# CPU and memory usage
top -p <PID>

# Or with fancy graphs
htop
btop

# Shared memory file
ls -lh /tmp/hal_pins.shm
```

### View Pin Changes

Daemon logs all pin changes to stdout:
```
[PIN-MON] Pin 0 (spindle.speed) changed: 0 -> 1500
[PIN-MON] Pin 1 (axis.0.pos-cmd) changed: 0 -> 12345
[PIN-MON] Pin 3 (estop.triggered) changed: 0 -> 1
```

### Heartbeat

Kernel prints heartbeat every 10,000 iterations:
```
[KERNEL] Heartbeat - 10000 iterations, 4 pins
```

## 🚧 Current Status

**✅ Implemented:**
- [x] Microkernel core with process isolation
- [x] Message passing system (circular queue)
- [x] Service registry and lifecycle management
- [x] Pin monitoring with change detection
- [x] Automatic service restart on crash
- [x] Adaptive sleep for CPU efficiency
- [x] Shared memory IPC (file-backed)
- [x] HAL bridge component (C)
- [x] CLI tools (poker, stress tester)
- [x] Stress tested to 400+ updates/sec

**🔄 In Progress:**
- [ ] Testing on actual LinuxCNC hardware
- [ ] Integration with real HAL configurations
- [ ] Service callback system for pin changes
- [ ] Extended pin types (bit, s32, u32)
- [ ] Pin direction handling (in/out/io)

**🔮 Future Plans:**
- [ ] IPC pipes between services
- [ ] Service dependency tracking
- [ ] Priority-based service scheduling
- [ ] Resource limits per service
- [ ] Web-based monitoring dashboard
- [ ] Python bindings for easy service development
- [ ] Configuration file support

## 🤝 Contributing

This is an experimental project exploring modern microkernel architectures for CNC control systems. Contributions, testing, and feedback welcome!

### Development Requirements

- **AILang Compiler** - For microkernel/tools
- **LinuxCNC 2.8+** - For HAL integration
- **GCC** - For bridge component
- **Linux** - x86-64 architecture

## 📝 Technical Notes

### Why File-Backed Shared Memory?

POSIX shared memory (`shm_open`) had compatibility issues on WSL2. File-backed memory mapping (`mmap`) is:
- More portable
- Works on all Linux variants
- Easy to inspect (`hexdump /tmp/hal_pins.shm`)
- Cleaned up automatically by OS

### Why AILang?

AILang compiles directly to optimized x86-64 assembly with:
- Zero runtime dependencies
- Predictable performance
- Tiny binaries (30KB)
- Direct syscall access
- No garbage collection overhead

### Why Process Isolation?

Traditional HAL components run in the RTAPI thread, meaning a crash or blocking call affects the entire system. Process isolation means:
- Service crashes don't affect kernel
- Services can use standard libraries
- Blocking operations are safe
- Easier debugging and development

## 📄 License

MIT

## 🙏 Acknowledgments

- LinuxCNC Project - For the excellent CNC control software
- AILang - For the modern systems programming language
- The CNC community - For inspiration and requirements

---

**Status:** Alpha - Stress tested but not yet deployed on production CNC hardware




