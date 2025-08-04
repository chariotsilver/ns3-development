# Classical Load Probe Implementation

## Overview
Successfully implemented a classical traffic load monitoring system to feed Raman/crosstalk effects into the QKD channel model. This allows the simulation to model how classical network traffic impacts quantum key distribution performance.

## Components Implemented

### 1. ClassicalLoadProbe Class
```cpp
class ClassicalLoadProbe {
public:
  ClassicalLoadProbe(Ptr<QkdFiberChannel> ch, double lambdaNm);
  void Report(double mbps);
private:
  Ptr<QkdFiberChannel> m_ch;
  double m_lambda;
};
```
- **Purpose**: Simple utility class to report classical traffic loads to QKD channel
- **Interface**: `Report(double mbps)` method feeds traffic data into `QkdFiberChannel::UpdateClassicalLoad()`
- **Configuration**: Associates probe with specific wavelength (λ) for crosstalk modeling

### 2. ClassicalLoadMonitor Application
```cpp
class ClassicalLoadMonitor : public Application {
public:
  ClassicalLoadMonitor(qkd::ClassicalLoadProbe* probe, Time period);
  // ns-3 Application lifecycle methods
private:
  void Monitor();  // Periodic traffic sampling
  qkd::ClassicalLoadProbe* m_probe;
  Time m_period;
  uint32_t m_counter;
  EventId m_event;
};
```
- **Purpose**: ns-3 Application that periodically monitors and reports classical traffic loads
- **Simulation Pattern**: Uses sinusoidal variation (20-100 Mbps) to simulate realistic traffic patterns
- **Period**: Configurable monitoring interval (default: 100ms)
- **Integration**: Proper ns-3 Application framework integration with Start/Stop lifecycle

### 3. Enhanced QkdFiberChannel
- **UpdateClassicalLoad()**: Existing method enhanced to accept classical traffic reports
- **Crosstalk Modeling**: Uses Gaussian-weighted Raman scattering model based on wavelength separation
- **Multi-wavelength Support**: Tracks traffic loads for multiple wavelengths simultaneously

## Integration Points

### QKD Setup Integration
```cpp
// Create classical load probe for 1530nm wavelength
qkd::ClassicalLoadProbe* classicalProbe = new qkd::ClassicalLoadProbe(fiberCh, 1530.0);

// Install monitoring application on Alice's node
Ptr<ClassicalLoadMonitor> monitor = Create<ClassicalLoadMonitor>(classicalProbe, MilliSeconds(100));
nodes.Get(qSrc)->AddApplication(monitor);
monitor->SetStartTime(Seconds(1.5));
monitor->SetStopTime(Seconds(simulationTime - 0.1));
```

## Validation Results

### Successful Operation Verified
1. **ClassicalLoadMonitor**: Regular traffic monitoring every 100ms with sinusoidal load variation
2. **Probe Integration**: Correct calls to `QkdFiberChannel::UpdateClassicalLoad()` with varying loads
3. **QKD Performance**: Continued realistic QKD window performance with QBER values around 2-3%
4. **Load Pattern**: Smooth sinusoidal variation (50 → 53 → 56 → 59 → 62 → 64 → 67 → 69 Mbps)

### Expected Effects
- **Raman Crosstalk**: Higher classical traffic loads increase quantum channel noise
- **Wavelength Dependency**: Gaussian-weighted impact based on wavelength separation
- **Dynamic Response**: Real-time adaptation of QKD performance to traffic conditions

## Usage Example

```cpp
// 1. Create QKD fiber channel (existing)
Ptr<QkdFiberChannel> fiberCh = Create<QkdFiberChannel>();

// 2. Create classical load probe for specific wavelength
qkd::ClassicalLoadProbe* probe = new qkd::ClassicalLoadProbe(fiberCh, 1530.0);

// 3. Create and install monitoring application
Ptr<ClassicalLoadMonitor> monitor = Create<ClassicalLoadMonitor>(probe, MilliSeconds(100));
alice_node->AddApplication(monitor);
monitor->SetStartTime(Seconds(1.0));
monitor->SetStopTime(Seconds(simDuration));

// 4. Manual load injection (optional)
fiberCh->UpdateClassicalLoad(1530.0, 1000.0);  // High load test
```

## Technical Implementation

### Key Features
- **ns-3 Integration**: Proper Application framework usage with event scheduling
- **Configurable Monitoring**: Adjustable period and wavelength parameters
- **Realistic Traffic Patterns**: Sinusoidal load variation simulating real network behavior
- **Multi-wavelength Support**: Can monitor multiple wavelengths simultaneously
- **Memory Management**: Proper cleanup in destructor

### Performance Impact
- **Low Overhead**: Minimal computational impact from 100ms monitoring intervals
- **Scalable**: Can support multiple probes for different wavelengths/channels
- **Event-driven**: Uses ns-3's efficient event scheduling system

## Next Steps
1. **Advanced Traffic Patterns**: Implement more sophisticated traffic models (burst patterns, protocol-specific loads)
2. **Real Interface Monitoring**: Replace simulation with actual network interface sampling
3. **Multi-channel Support**: Extend to monitor multiple fiber channels simultaneously
4. **Adaptive QKD**: Implement QKD parameter adjustment based on classical load conditions
