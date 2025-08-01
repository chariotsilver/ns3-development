#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/pfifo-fast-queue-disc.h"
#include "ns3/queue-size.h"
#include "ns3/ofswitch13-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/random-variable-stream.h"
#include "ns3/rng-seed-manager.h"
#include "ns3/type-id.h"
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>
#include <atomic>

using namespace ns3;

// Link Failure and Recovery Module for Network Robustness Testing
enum class FailureType {
  FIBER_CUT,      // Complete link failure
  TRANSIENT,      // Temporary failure (flapping)
  DEGRADATION     // Gradual performance degradation
};

struct LinkFailureEvent {
  Time scheduleTime;
  FailureType type;
  uint32_t linkId;
  std::string description;
  double parameter1;  // For degradation: loss rate
  double parameter2;  // For degradation: additional delay
};

class LinkFailureModule {
public:
  LinkFailureModule() {
    m_failureLog.open("link_failures.log");
    m_failureLog << "# Time,Event,Type,LinkId,Description,Parameter1,Parameter2\n";
    m_linkIdCounter = 0;
    m_enableFailures = false;
  }

  ~LinkFailureModule() {
    if (m_failureLog.is_open()) {
      m_failureLog.close();
    }
  }

  void EnableFailures(bool enable) { m_enableFailures = enable; }

  void RegisterLink(NetDeviceContainer link, const std::string& description = "") {
    uint32_t linkId = m_linkIdCounter++;
    m_coreLinks[linkId] = link;
    m_linkDescriptions[linkId] = description.empty() 
      ? ("Link_" + std::to_string(linkId)) : description;
    m_linkStates[linkId] = true; // Initially operational
    
    NS_LOG_INFO("LinkFailureModule: Registered link " << linkId 
                << " (" << m_linkDescriptions[linkId] << ")");
  }

  void ScheduleRealisticFailures() {
    if (!m_enableFailures) {
      NS_LOG_INFO("LinkFailureModule: Failure simulation disabled");
      return;
    }

    if (m_coreLinks.size() < 4) {
      NS_LOG_WARN("LinkFailureModule: Need at least 4 links for realistic failure scenarios");
      return;
    }

    NS_LOG_INFO("LinkFailureModule: Scheduling realistic failure scenarios...");

    // Scenario 1: Fiber cut simulation (complete failure)
    ScheduleEvent({
      Seconds(30.0),
      FailureType::FIBER_CUT,
      2,
      "Primary_Core_Link_Failure",
      0.0, 0.0
    });

    // Scenario 2: Gradual degradation (increasing packet loss)
    ScheduleEvent({
      Seconds(20.0),
      FailureType::DEGRADATION,
      1,
      "Link_Quality_Degradation",
      0.001,  // Initial loss rate: 0.1%
      5.0     // Additional delay: 5ms
    });

    // Enhanced degradation over time
    ScheduleEvent({
      Seconds(35.0),
      FailureType::DEGRADATION,
      1,
      "Severe_Link_Degradation",
      0.01,   // Increased loss rate: 1%
      15.0    // Additional delay: 15ms
    });

    // Scenario 3: Flapping link (intermittent failures)
    Time flapStart = Seconds(40.0);
    for (int i = 0; i < 10; ++i) {
      FailureType type = (i % 2 == 0) ? FailureType::TRANSIENT : FailureType::TRANSIENT;
      ScheduleEvent({
        flapStart + Seconds(i * 3.0),
        type,
        3,
        i % 2 == 0 ? "Link_Flap_Down" : "Link_Flap_Up",
        0.0, 0.0
      });
    }

    // Scenario 4: Recovery test - restore fiber cut link
    ScheduleEvent({
      Seconds(80.0),
      FailureType::TRANSIENT, // Using TRANSIENT type for restoration
      2,
      "Fiber_Cut_Recovery",
      0.0, 0.0
    });

    // Scenario 5: Multiple simultaneous failures (stress test)
    if (m_coreLinks.size() >= 6) {
      ScheduleEvent({
        Seconds(90.0),
        FailureType::FIBER_CUT,
        4,
        "Simultaneous_Failure_1",
        0.0, 0.0
      });

      ScheduleEvent({
        Seconds(90.5),
        FailureType::FIBER_CUT,
        5,
        "Simultaneous_Failure_2",
        0.0, 0.0
      });
    }

    NS_LOG_INFO("LinkFailureModule: Scheduled " << m_scheduledEvents.size() << " failure events");
  }

  void PrintFailureSummary() {
    std::cout << "\n=== Link Failure Summary ===" << std::endl;
    std::cout << "Total registered links: " << m_coreLinks.size() << std::endl;
    std::cout << "Scheduled failure events: " << m_scheduledEvents.size() << std::endl;
    
    uint32_t operational = 0;
    for (const auto& [linkId, state] : m_linkStates) {
      if (state) operational++;
    }
    
    std::cout << "Currently operational links: " << operational 
              << "/" << m_linkStates.size() << std::endl;
    std::cout << "Failure log: link_failures.log" << std::endl;
    std::cout << "===========================" << std::endl;
  }

private:
  void ScheduleEvent(const LinkFailureEvent& event) {
    m_scheduledEvents.push_back(event);
    
    Simulator::Schedule(event.scheduleTime, [this, event]() {
      ExecuteFailureEvent(event);
    });
  }

  void ExecuteFailureEvent(const LinkFailureEvent& event) {
    auto linkIt = m_coreLinks.find(event.linkId);
    if (linkIt == m_coreLinks.end()) {
      NS_LOG_ERROR("LinkFailureModule: Link " << event.linkId << " not found");
      return;
    }

    NetDeviceContainer& link = linkIt->second;
    bool currentState = m_linkStates[event.linkId];

    switch (event.type) {
      case FailureType::FIBER_CUT:
        if (currentState) {
          FailLink(link, event.linkId, "FIBER_CUT");
        } else {
          RestoreLink(link, event.linkId, "FIBER_RESTORE");
        }
        break;

      case FailureType::TRANSIENT:
        if (event.description.find("Up") != std::string::npos || 
            event.description.find("Recovery") != std::string::npos) {
          RestoreLink(link, event.linkId, "TRANSIENT_RESTORE");
        } else {
          FailLink(link, event.linkId, "TRANSIENT_FAIL");
        }
        break;

      case FailureType::DEGRADATION:
        DegradeLink(link, event.linkId, event.parameter1, event.parameter2);
        break;
    }

    // Log the event
    LogFailureEvent(event);
  }

  void FailLink(NetDeviceContainer& link, uint32_t linkId, const std::string& reason) {
    if (!m_linkStates[linkId]) {
      NS_LOG_DEBUG("LinkFailureModule: Link " << linkId << " already failed");
      return;
    }

    // Disable transmission on both ends of the link
    Ptr<CsmaNetDevice> dev0 = DynamicCast<CsmaNetDevice>(link.Get(0));
    Ptr<CsmaNetDevice> dev1 = DynamicCast<CsmaNetDevice>(link.Get(1));

    if (dev0) {
      // Disable the device by setting it to a non-transmitting state
      dev0->GetQueue()->SetMaxSize(QueueSize("0p"));
    }
    if (dev1) {
      dev1->GetQueue()->SetMaxSize(QueueSize("0p"));
    }

    m_linkStates[linkId] = false;

    NS_LOG_WARN("LinkFailureModule: FAILED link " << linkId 
                << " (" << m_linkDescriptions[linkId] << ") - " << reason);

    // Notify controller about link state change
    NotifyController(linkId, false);
  }

  void RestoreLink(NetDeviceContainer& link, uint32_t linkId, const std::string& reason) {
    if (m_linkStates[linkId]) {
      NS_LOG_DEBUG("LinkFailureModule: Link " << linkId << " already operational");
      return;
    }

    // Re-enable transmission on both ends
    Ptr<CsmaNetDevice> dev0 = DynamicCast<CsmaNetDevice>(link.Get(0));
    Ptr<CsmaNetDevice> dev1 = DynamicCast<CsmaNetDevice>(link.Get(1));

    if (dev0) {
      dev0->GetQueue()->SetMaxSize(QueueSize("100p")); // Restore default queue size
    }
    if (dev1) {
      dev1->GetQueue()->SetMaxSize(QueueSize("100p"));
    }

    // Remove any error models that might have been added
    Ptr<CsmaNetDevice> csma0 = DynamicCast<CsmaNetDevice>(link.Get(0));
    Ptr<CsmaNetDevice> csma1 = DynamicCast<CsmaNetDevice>(link.Get(1));
    if (csma0) csma0->SetReceiveErrorModel(nullptr);
    if (csma1) csma1->SetReceiveErrorModel(nullptr);

    m_linkStates[linkId] = true;

    NS_LOG_INFO("LinkFailureModule: RESTORED link " << linkId 
                << " (" << m_linkDescriptions[linkId] << ") - " << reason);

    // Notify controller about link restoration
    NotifyController(linkId, true);
  }

  void DegradeLink(NetDeviceContainer& link, uint32_t linkId, double lossRate, double additionalDelay) {
    NS_LOG_INFO("LinkFailureModule: DEGRADING link " << linkId 
                << " loss=" << lossRate << " delay=+" << additionalDelay << "ms");

    // Add packet loss error model
    Ptr<RateErrorModel> errorModel0 = CreateObject<RateErrorModel>();
    errorModel0->SetRate(lossRate);
    errorModel0->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

    Ptr<RateErrorModel> errorModel1 = CreateObject<RateErrorModel>();
    errorModel1->SetRate(lossRate);
    errorModel1->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

    // Cast to CsmaNetDevice which supports error models
    Ptr<CsmaNetDevice> dev0 = DynamicCast<CsmaNetDevice>(link.Get(0));
    Ptr<CsmaNetDevice> dev1 = DynamicCast<CsmaNetDevice>(link.Get(1));

    if (dev0) {
      dev0->SetReceiveErrorModel(errorModel0);
    }
    if (dev1) {
      dev1->SetReceiveErrorModel(errorModel1);
    }

    // Note: Adding delay variation would require custom channel implementation
    // For now, we log the intended delay increase for analysis
    
    m_failureLog << Simulator::Now().GetSeconds() 
                 << ",LINK_DEGRADE,QUALITY_LOSS," << linkId 
                 << "," << m_linkDescriptions[linkId]
                 << "," << lossRate << "," << additionalDelay << "\n";
  }

  void NotifyController(uint32_t linkId, bool isUp) {
    // In a real implementation, this would trigger OpenFlow port status messages
    // For simulation purposes, we log the event for controller awareness
    std::string status = isUp ? "PORT_UP" : "PORT_DOWN";
    
    m_failureLog << Simulator::Now().GetSeconds() 
                 << ",PORT_STATUS," << status << "," << linkId
                 << "," << m_linkDescriptions[linkId] 
                 << ",0,0\n";

    NS_LOG_INFO("LinkFailureModule: Notified controller - Link " << linkId 
                << " status: " << status);
  }

  void LogFailureEvent(const LinkFailureEvent& event) {
    std::string typeStr;
    switch (event.type) {
      case FailureType::FIBER_CUT: typeStr = "FIBER_CUT"; break;
      case FailureType::TRANSIENT: typeStr = "TRANSIENT"; break;
      case FailureType::DEGRADATION: typeStr = "DEGRADATION"; break;
    }

    m_failureLog << Simulator::Now().GetSeconds() 
                 << ",SCHEDULED_EVENT," << typeStr << "," << event.linkId
                 << "," << event.description
                 << "," << event.parameter1 << "," << event.parameter2 << "\n";
    m_failureLog.flush();
  }

private:
  std::unordered_map<uint32_t, NetDeviceContainer> m_coreLinks;
  std::unordered_map<uint32_t, std::string> m_linkDescriptions;
  std::unordered_map<uint32_t, bool> m_linkStates;
  std::vector<LinkFailureEvent> m_scheduledEvents;
  std::ofstream m_failureLog;
  uint32_t m_linkIdCounter;
  bool m_enableFailures;
};

// Control plane metrics tracking
struct ControlPlaneMetrics {
  static inline Time firstSwitchTime = Time(0);
  static inline uint32_t batchesProcessed = 0;
  static inline uint32_t flowsInstalled = 0;
  static inline Time lastFlowTime = Time(0);
  
  static void RecordFirstSwitch() {
    if (firstSwitchTime == Time(0)) {
      firstSwitchTime = Simulator::Now();
    }
  }
  
  static void RecordBatch() {
    batchesProcessed++;
  }
  
  static void RecordFlowInstall() {
    flowsInstalled++;
    lastFlowTime = Simulator::Now();
  }
  
  static void PrintSummary() {
    Time convergenceTime = lastFlowTime - firstSwitchTime;
    std::cout << "\n--- Control Plane Metrics ---" << std::endl;
    std::cout << "First switch connected: " << firstSwitchTime.GetSeconds() << "s" << std::endl;
    std::cout << "Last flow installed: " << lastFlowTime.GetSeconds() << "s" << std::endl;
    std::cout << "Convergence time: " << convergenceTime.GetMilliSeconds() << "ms" << std::endl;
    std::cout << "Total batches: " << batchesProcessed << std::endl;
    std::cout << "Total flows: " << flowsInstalled << std::endl;
    std::cout << "----------------------------\n" << std::endl;
  }
};

NS_LOG_COMPONENT_DEFINE("SpProactiveController");

// Global variables for error tracking and verbosity
static std::atomic<uint32_t> g_dpctlErrors{0};
static uint32_t g_verbosity = 1;  // Default verbosity level

// Global link failure module for robustness testing
static LinkFailureModule g_linkFailures;

// Returns true if this build exposes the "Limit" attribute on PfifoFastQueueDisc
static bool PfifoHasLimitAttr() {
  ns3::TypeId tid = ns3::PfifoFastQueueDisc::GetTypeId();
  ns3::TypeId::AttributeInformation info;
  return tid.LookupAttributeByName("Limit", &info);
}

// ---- Proactive Shortest-Path Controller ----

// IDs we'll use internally
using Sw = uint64_t;     // switch id (DPID) or Node->GetId() mapped to DPID
using Port = uint32_t;   // OpenFlow port number (1-based)
using Host = uint32_t;   // host index (h0,h1,...)

struct HostInfo {
  Mac48Address mac;
  Ipv4Address  ip;
  Sw           edgeSw;
  Port         edgePort;  // port on edge switch facing the host
};

struct PortPeer {
  bool isSwitch{false};
  Sw   sw{};
  Host host{};
};

// Switch adjacency: on switch S, port p leads to either another switch or a host
using PortMap = std::map<Port, PortPeer>;
using SwAdj   = std::unordered_map<Sw, PortMap>;

class SpProactiveController : public OFSwitch13Controller {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("SpProactiveController")
      .SetParent<OFSwitch13Controller>()
      .SetGroupName("OFSwitch13")
      .AddConstructor<SpProactiveController>();
    return tid;
  }

  // Inject topology and hosts before StartApplication()
  void SetAdjacency(const SwAdj& adj) { m_adj = adj; }
  void SetHosts(const std::vector<HostInfo>& hosts) { m_hosts = hosts; }

  // (no-op: remapping done in main now)

protected:
  virtual void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override {
    uint64_t dpid = swtch->GetDpId();
    NS_LOG_INFO("SpProactiveController: switch connected DPID=" << dpid);
    
    InstallTableMissDrop(dpid);
    PushFlowsForSwitch(dpid);
  }

protected:
  // helpers - moved to protected for subclass access
  static bool DpctlFailed(int rc) { return rc != 0; }
  static bool DpctlFailed(const std::string& s) {
    auto has = [&](const char* k){ return s.find(k) != std::string::npos; };
    return has("error") || has("Error") || has("invalid") || has("failed");
  }
  static std::string DpctlToString(int rc) { return std::to_string(rc); }
  static std::string DpctlToString(const std::string& s) { return s; }

private:
  // Safe dpctl wrapper for better error handling
  void DpctlOrWarn(const char* where, uint64_t dpid, const std::string& cmd) {
    auto out = DpctlExecute(dpid, cmd); // int OR std::string
    if (DpctlFailed(out)) {
      ++g_dpctlErrors;
      NS_LOG_WARN(where << ": dpctl problem dpid=" << std::hex << dpid
                        << " cmd='" << cmd << "' -> " << DpctlToString(out));
    } else {
      NS_LOG_DEBUG(where << ": dpctl ok dpid=" << std::hex << dpid
                         << " cmd='" << cmd << "'");
    }
  }

protected:
  void InstallTableMissDrop(uint64_t dpid) {
    // Install lowest priority rule to drop unmatched packets with explicit empty instruction list
    DpctlOrWarn("table-miss", dpid, "flow-mod cmd=add,table=0,prio=0 apply:");
  }

  void PushFlowsForSwitch(uint64_t dpid) {
    NS_LOG_DEBUG("push flows for DPID=" << dpid << " (hosts=" << m_hosts.size() << ")");
    
    // Install ARP and IPv4 rules for all hosts that can reach this switch
    for (const auto& host : m_hosts) {
      Port outPort;
      if (dpid == host.edgeSw) {
        outPort = host.edgePort; // Direct delivery to host
        NS_LOG_DEBUG("host " << host.ip << " directly on " << dpid << " port " << outPort);
      } else {
        if (!NextHopPort(dpid, host.edgeSw, outPort)) {
          NS_LOG_WARN("no path from DPID " << dpid << " to host " << host.ip
                     << " (edgeSw=" << host.edgeSw << ")");
          continue;
        }
        NS_LOG_DEBUG("host " << host.ip << " via DPID " << dpid << " out port " << outPort);
      }
      
      // Install ARP rule
      std::ostringstream arpCmd;
      arpCmd << "flow-mod cmd=add,table=0,prio=200"
             << " eth_type=0x0806,arp_tpa=" << host.ip
             << " apply:output=" << outPort;
      DpctlOrWarn("ARP", dpid, arpCmd.str());

      // Install IPv4 rule
      std::ostringstream ipCmd;
      ipCmd << "flow-mod cmd=add,table=0,prio=100"
            << " eth_type=0x0800,eth_dst=" << host.mac
            << " apply:output=" << outPort;
      DpctlOrWarn("IPv4", dpid, ipCmd.str());

      // One-time next-hop log per (switch, host-IP)
      if (m_loggedNextHop.emplace(dpid, host.ip.Get()).second) {
        NS_LOG_INFO("NextHop: sw=" << std::hex << dpid << " -> " << host.ip
                     << " via port " << std::dec << outPort);
      }
    }
  }

  bool NextHopPort(Sw src, Sw dst, Port& outPort) {
    if (src == dst) return false;

    std::queue<Sw> q;
    std::unordered_map<Sw, Sw> parent;
    std::unordered_set<Sw> visited;

    q.push(src);
    visited.insert(src);
    parent[src] = src;

    while (!q.empty()) {
      Sw curr = q.front();
      q.pop();

      if (curr == dst) {
        // Trace back to find next hop
        Sw next = dst;
        while (parent[next] != src) {
          next = parent[next];
        }
        auto it = m_adj.find(src);
        if (it != m_adj.end()) {
          for (const auto& [port, peer] : it->second) {
            if (peer.isSwitch && peer.sw == next) {
              outPort = port;
              return true;
            }
          }
        }
        return false;
      }

      auto it = m_adj.find(curr);
      if (it != m_adj.end()) {
        for (const auto& [port, peer] : it->second) {
          if (peer.isSwitch && visited.find(peer.sw) == visited.end()) {
            visited.insert(peer.sw);
            parent[peer.sw] = curr;
            q.push(peer.sw);
          }
        }
      }
    }
    // No path found: warn once per (src,dst)
    auto key = std::make_pair(src, dst);
    if (m_warnedNoPath.insert(key).second) {
      NS_LOG_WARN("NextHopPort: no path from " << src << " to " << dst);
    }
    return false;
  }

protected:
  SwAdj m_adj;
  std::vector<HostInfo> m_hosts;

private:
  std::set<std::pair<Sw,Sw>> m_warnedNoPath;
  std::set<std::pair<Sw,uint32_t>> m_loggedNextHop;
};

// ---- Realistic Controller with Control-Plane Latency Emulation ----

class RealisticController : public SpProactiveController {
public:
  static TypeId GetTypeId() {
    static TypeId tid = TypeId("RealisticController")
      .SetParent<SpProactiveController>()
      .SetGroupName("OFSwitch13")
      .AddConstructor<RealisticController>();
    return tid;
  }

  RealisticController() : m_controlPlaneDelay(MilliSeconds(5)) {
    m_processingRng = CreateObject<UniformRandomVariable>();
    m_processingRng->SetAttribute("Min", DoubleValue(2.0));
    m_processingRng->SetAttribute("Max", DoubleValue(8.0));
  }

protected:
  virtual void HandshakeSuccessful(Ptr<const RemoteSwitch> swtch) override {
    uint64_t dpid = swtch->GetDpId();
    NS_LOG_INFO("RealisticController: Switch " << dpid << " connected at " 
                << Simulator::Now().GetSeconds() << "s");
    
    // Record first switch connection for convergence metrics
    ControlPlaneMetrics::RecordFirstSwitch();
    
    // Emulate controller processing delay
    Time processingDelay = MilliSeconds(m_processingRng->GetValue());
    
    Simulator::Schedule(processingDelay, [this, dpid]() {
      InstallTableMissDrop(dpid);
      // Stagger flow installations to prevent control plane flooding
      PushFlowsWithRateLimit(dpid);
    });
  }

private:
  void PushFlowsWithRateLimit(uint64_t dpid) {
    const uint32_t FLOWS_PER_BATCH = 50;
    const Time BATCH_INTERVAL = MilliSeconds(10);
    
    auto flowsToInstall = ComputeFlowsForSwitch(dpid);
    
    for (size_t i = 0; i < flowsToInstall.size(); i += FLOWS_PER_BATCH) {
      Time delay = BATCH_INTERVAL * (i / FLOWS_PER_BATCH);
      
      Simulator::Schedule(delay + m_controlPlaneDelay, [this, dpid, flowsToInstall, i]() {
        ControlPlaneMetrics::RecordBatch();
        
        size_t end = std::min(i + FLOWS_PER_BATCH, flowsToInstall.size());
        for (size_t j = i; j < end; ++j) {
          DpctlExecuteDelayed(dpid, flowsToInstall[j]);
        }
        
        NS_LOG_DEBUG("Installed flow batch " << i/FLOWS_PER_BATCH 
                     << " on switch " << dpid);
      });
    }
  }

  std::vector<std::string> ComputeFlowsForSwitch(uint64_t dpid) {
    std::vector<std::string> flows;
    
    // Compute all flows that need to be installed for this switch
    for (const auto& host : m_hosts) {
      Port outPort;
      if (dpid == host.edgeSw) {
        outPort = host.edgePort; // Direct delivery to host
      } else {
        if (!NextHopPort(dpid, host.edgeSw, outPort)) {
          continue; // No path available
        }
      }
      
      // Create ARP rule command
      std::ostringstream arpCmd;
      arpCmd << "flow-mod cmd=add,table=0,prio=200"
             << " eth_type=0x0806,arp_tpa=" << host.ip
             << " apply:output=" << outPort;
      flows.push_back(arpCmd.str());

      // Create IPv4 rule command  
      std::ostringstream ipCmd;
      ipCmd << "flow-mod cmd=add,table=0,prio=100"
            << " eth_type=0x0800,eth_dst=" << host.mac
            << " apply:output=" << outPort;
      flows.push_back(ipCmd.str());
    }
    
    NS_LOG_INFO("Computed " << flows.size() << " flows for switch " << dpid);
    return flows;
  }

  void DpctlExecuteDelayed(uint64_t dpid, const std::string& cmd) {
    // Add small random jitter to simulate OpenFlow message processing
    Time jitter = MicroSeconds(m_processingRng->GetValue() * 100);
    
    Simulator::Schedule(jitter, [this, dpid, cmd]() {
      ControlPlaneMetrics::RecordFlowInstall();
      
      auto out = DpctlExecute(dpid, cmd);
      if (DpctlFailed(out)) {
        ++g_dpctlErrors;
        NS_LOG_WARN("RealisticController: dpctl problem dpid=" << std::hex << dpid
                    << " cmd='" << cmd << "' -> " << DpctlToString(out));
      } else {
        NS_LOG_DEBUG("RealisticController: dpctl ok dpid=" << std::hex << dpid);
      }
    });
  }

private:
  Time m_controlPlaneDelay;
  Ptr<UniformRandomVariable> m_processingRng;
};

// Telemetry callbacks for queue monitoring
void QdLen(uint32_t oldVal, uint32_t newVal) {
  std::cout << Simulator::Now().GetSeconds() << ",QLEN,," << newVal << "\n";
}

void QdDrop(Ptr<const QueueDiscItem> item) {
  std::cout << Simulator::Now().GetSeconds() << ",DROP,,1\n";
}

static void QdLenTagged(std::string tag, uint32_t oldVal, uint32_t newVal) {
  std::cout << Simulator::Now().GetSeconds() << ",QLEN," << tag << "," << newVal << "\n";
}

static void QdDropTagged(std::string tag, Ptr<const QueueDiscItem>) {
  std::cout << Simulator::Now().GetSeconds() << ",DROP," << tag << ",1\n";
}

static void PollQ(QueueDiscContainer qds, Time interval) {
  for (uint32_t i = 0; i < qds.GetN(); ++i) {
    auto qd = qds.Get(i);
    std::cout << Simulator::Now().GetSeconds()
              << ",QSIZE_OBJ," << qd->GetNPackets()
              << ",BYTES=" << qd->GetNBytes() << "\n";
  }
  Simulator::Schedule(interval, &PollQ, qds, interval);
}

  class QkdWindowApp : public ns3::Application {
  public:
    void Configure(Ptr<Node> n, Ipv4Address dst, uint16_t dport,
                  Time start, Time dur, uint32_t pktSize, double pps, uint8_t tos = 0xC0) {
      m_node=n; m_dst=dst; m_dport=dport; m_start=start; m_dur=dur;
      m_pktSize=pktSize; m_interval=Seconds(1.0/pps); m_tos=tos;
    }
  private:
    void StartApplication() override {
      m_sock = Socket::CreateSocket(m_node, UdpSocketFactory::GetTypeId());
      m_sock->SetPriority(6);   // High priority band for reliable classification
      m_sock->SetIpTos(m_tos);   // Configurable QoS marking (CS6 or EF)
      m_sock->Connect(InetSocketAddress(m_dst, m_dport));
      Simulator::Schedule(m_start, &QkdWindowApp::StartBurst, this);
      Simulator::Schedule(m_start + m_dur, &QkdWindowApp::StopBurst, this);
    }
    void StopApplication() override { if (m_sock) { m_sock->Close(); m_sock=0; } }
    void StartBurst() { m_on=true; SendOne(); }
    void StopBurst()  { m_on=false; }
    void SendOne() {
      if (!m_on) return;
      m_sock->Send(Create<Packet>(m_pktSize));
      Simulator::Schedule(m_interval, &QkdWindowApp::SendOne, this);
    }
    Ptr<Node> m_node; Ptr<Socket> m_sock;
    Ipv4Address m_dst; uint16_t m_dport{5555};
    Time m_start, m_dur, m_interval; uint32_t m_pktSize{400}; bool m_on{false};
    uint8_t m_tos{0xC0};  // Default to CS6
  };

  static NetDeviceContainer Link(Ptr<Node>a, Ptr<Node>b, std::string rate, std::string delay, uint32_t txQueueMaxP = 25)
  {
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue(rate));
    csma.SetChannelAttribute("Delay",   StringValue(delay));
    csma.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", StringValue(std::to_string(txQueueMaxP) + "p"));
    return csma.Install(NodeContainer(a,b));
  }

  struct Man {
    std::vector< Ptr<Node> > sw;
    std::vector< Ptr<Node> > host;
    std::vector< NetDeviceContainer > coreLinks;  // ring links
    std::vector< NetDeviceContainer > hostLinks;  // host<->switch
  };

  struct Edge {
    Ptr<Node> a, b;
    NetDeviceContainer devs;
    std::string kind; // "core" or "spur"
    std::string nameA, nameB;
  };

  struct TopoBuild {
    std::vector< Ptr<Node> > sw, host;
    std::vector<Edge> coreEdges, spurEdges;
    std::map<std::string, Ptr<Node>> nameToNode;
    std::vector<std::string> hostNames;
  };

  // helpers to recognize node names
  static bool IsSwitchName(const std::string& n){ return !n.empty() && n[0]=='s'; }
  static bool IsHostName  (const std::string& n){ return !n.empty() && n[0]=='h'; }

  TopoBuild BuildFromCsv(const std::string& path, uint32_t txQueueMaxP)
  {
    TopoBuild tb;

    auto getNode = [&](const std::string& name)->Ptr<Node>{
      auto it = tb.nameToNode.find(name);
      if (it != tb.nameToNode.end()) return it->second;
      Ptr<Node> n = CreateObject<Node>();
      tb.nameToNode[name] = n;
      if (IsSwitchName(name)) {
        tb.sw.push_back(n);
      } else if (IsHostName(name)) {
        tb.host.push_back(n);
        tb.hostNames.push_back(name);
      }
      return n;
    };

    std::ifstream fin(path);
    if (!fin) { NS_FATAL_ERROR("Cannot open topo CSV: " << path); }
    std::string line;
    while (std::getline(fin, line)) {
      if (line.empty() || line[0]=='#') continue;
      std::stringstream ss(line);
      std::string u,v,kind,rate,delay;
      std::getline(ss,u,','); std::getline(ss,v,',');
      std::getline(ss,kind,','); std::getline(ss,rate,','); std::getline(ss,delay,',');
      
      // Validate edge kind
      if (kind != "core" && kind != "spur") {
        NS_FATAL_ERROR("Unknown CSV kind='" << kind << "' on edge " << u << "," << v);
      }
      
      Ptr<Node> nu = getNode(u), nv = getNode(v);
      auto devs = Link(nu, nv, rate, delay, txQueueMaxP);
      Edge e; e.a=nu; e.b=nv; e.devs=devs; e.kind=kind;
      e.nameA = u; e.nameB = v;  // Track node names for robust device selection
      if (kind=="core") tb.coreEdges.push_back(e); else tb.spurEdges.push_back(e);
    }
    return tb;
  }

  Man BuildMan(uint32_t nCore, std::string coreRate, std::string coreDelay,
              std::string spurRate, std::string spurDelay, bool enableRing = false, uint32_t txQueueMaxP = 25)
  {
    Man m; 
    m.sw.resize(nCore); 
    m.host.resize(nCore);
    
    // Create nodes
    for (uint32_t i = 0; i < nCore; i++) { 
      m.sw[i] = CreateObject<Node>(); 
      m.host[i] = CreateObject<Node>(); 
    }
    
    // Host spur links (CSMA)
    for (uint32_t i = 0; i < nCore; i++) {
      m.hostLinks.push_back(Link(m.host[i], m.sw[i], spurRate, spurDelay, txQueueMaxP));
    }
    
    // Core topology: line by default, ring only if requested (CSMA)
    for (uint32_t i = 0; i < nCore - 1; i++) {
      auto a = m.sw[i];
      auto b = m.sw[i + 1];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay, txQueueMaxP));
    }
    
    // Close the ring only if explicitly enabled AND we have >2 switches
    if (enableRing && nCore > 2) {
      auto a = m.sw[nCore - 1];
      auto b = m.sw[0];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay, txQueueMaxP));
    }
    
    return m;
  }

  // Helper functions for DPID discovery and remapping (called from main)
  static std::unordered_map<uint32_t, Sw>
  BuildNodeToDpid(const std::vector<Ptr<Node>>& swNodes) {
    std::unordered_map<uint32_t, Sw> m;
    for (auto swNode : swNodes) {
      Ptr<OFSwitch13Device> ofdev;
      
      // First try GetObject (aggregated objects) which is the most common case
      ofdev = swNode->GetObject<OFSwitch13Device>();
      
      // If not found as aggregated object, search through all devices
      if (!ofdev) {
        for (uint32_t i = 0; i < swNode->GetNDevices(); ++i) {
          ofdev = DynamicCast<OFSwitch13Device>(swNode->GetDevice(i));
          if (ofdev) break;
        }
      }
      
      NS_ABORT_MSG_IF(!ofdev, "No OFSwitch13Device on node " << swNode->GetId());
      const Sw dpid = ofdev->GetDatapathId();
      m[swNode->GetId()] = dpid;
      std::cout << "DEBUG: Discovered NodeId " << swNode->GetId() << " -> DPID " << dpid << std::endl;
    }
    return m;
  }

  static void RemapToDpid(
    const SwAdj& adjByNodeId, const std::vector<HostInfo>& hostsByNodeId,
    const std::unordered_map<uint32_t, Sw>& nodeToDpid,
    SwAdj& adjByDpid, std::vector<HostInfo>& hostsByDpid)
  {
    adjByDpid.clear(); 
    hostsByDpid = hostsByNodeId;
    
    for (const auto& kv : adjByNodeId) {
      Sw src = nodeToDpid.at(kv.first);
      for (const auto& pkv : kv.second) {
        Port p = pkv.first; 
        auto peer = pkv.second; 
        auto np = peer;
        if (peer.isSwitch) {
          np.sw = nodeToDpid.at((uint32_t)peer.sw);
        }
        adjByDpid[src][p] = np;
      }
    }
    
    for (auto& h : hostsByDpid) {
      h.edgeSw = nodeToDpid.at((uint32_t)h.edgeSw);
    }
    
    std::cout << "DEBUG: Remapped topology from NodeId to DPID keys" << std::endl;
  }

  // Generic adjacency and host builder that works for both built-in and CSV topologies
  static void BuildAdjAndHosts_Generic(
    bool usingCsv,
    const std::vector< NetDeviceContainer >& coreLinks,
    const std::vector< NetDeviceContainer >& spurLinks,
    const std::map<Ptr<NetDevice>, std::pair<uint32_t, Port>>& devToPortByNode,
    const std::map<Ptr<NetDevice>, Ipv4Address>& devToIp,
    SwAdj& adj,
    std::vector<HostInfo>& hosts)
  {
    adj.clear(); hosts.clear();

    // S–S links: both ends will be in devToPortByNode (because both are switch NICs)
    for (const auto &link : coreLinks) {
      auto a = link.Get(0), b = link.Get(1);
      auto [aNode, aPort] = devToPortByNode.at(a);
      auto [bNode, bPort] = devToPortByNode.at(b);
      // We'll swap to real DPID in §4
      adj[aNode][aPort] = PortPeer{true, bNode, 0};
      adj[bNode][bPort] = PortPeer{true, aNode, 0};
    }

    // Host spurs: one end is in devToPortByNode (switch side), the other isn't (host NIC)
    for (const auto &spur : spurLinks) {
      Ptr<NetDevice> d0 = spur.Get(0), d1 = spur.Get(1);
      Ptr<NetDevice> swNic = nullptr, hostNic = nullptr;
      auto it0 = devToPortByNode.find(d0), it1 = devToPortByNode.find(d1);

      if (it0 != devToPortByNode.end() && it1 == devToPortByNode.end()) {
        swNic = d0; hostNic = d1;
      } else if (it1 != devToPortByNode.end() && it0 == devToPortByNode.end()) {
        swNic = d1; hostNic = d0;
      } else {
        NS_FATAL_ERROR("spur link did not look like host<->switch");
      }

      auto [swNode, swPort] = devToPortByNode.at(swNic);
      Mac48Address mac = Mac48Address::ConvertFrom(hostNic->GetAddress());
      Ipv4Address  ip  = devToIp.at(hostNic);

      hosts.push_back( HostInfo{ mac, ip, /*edgeSw*/ Sw(swNode), /*edgePort*/ swPort } );
    }
  }

  int main(int argc, char** argv)
  {
    // Reproducibility
    uint32_t seed = 1, run = 1;
    
    // MAN parameters
    uint32_t nCore = 8;
    
    // QKD parameters
    uint32_t qSrc = 0, qDst = 1;
    double qkdStart = 1.0, qkdDur = 0.5; 
    uint32_t qkdPps = 5000;  // Reduced from 100k to prevent event loop stress
    
    // Best-effort parameters
    std::string beRate = "10Mbps";  // Reduced from 50Mbps to prevent overwhelming
    
    // Topology parameters
    std::string topoPath = "";
    bool enableRing = false;
    
    // Queue parameters
    uint32_t qdiscMaxP = 100, txQueueMaxP = 25;
    uint32_t qdiscPollMs = 20; // Higher default for scalability on larger topologies
    bool qdiscOnSwitch = false; // Switch-egress contention modeling
    
    // QoS parameters
    std::string qosMark = "CS6"; // or "EF"
    
    // Controller parameters
    bool realisticController = true;  // Use realistic control plane delays
    
    // Link failure parameters
    bool enableLinkFailures = false;  // Enable link failure simulation
    
    CommandLine cmd;
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("run", "RNG run number", run);
    cmd.AddValue("nCore", "Number of core switches", nCore);
    cmd.AddValue("topo", "CSV edge list (u,v,kind,rate,delay)", topoPath);
    cmd.AddValue("ring", "Enable ring topology (creates loops!)", enableRing);
    cmd.AddValue("qSrc", "QKD source host index", qSrc);
    cmd.AddValue("qDst", "QKD destination host index", qDst);
    cmd.AddValue("qkdStart", "QKD window start (s)", qkdStart);
    cmd.AddValue("qkdDur", "QKD window duration (s)", qkdDur);
    cmd.AddValue("qkdPps", "QKD packets per second", qkdPps);
    cmd.AddValue("beRate", "Best-effort data rate", beRate);
    cmd.AddValue("qdiscMaxP", "PfifoFast per-band packet limit (packets). Applied only if supported in this ns-3 build.", qdiscMaxP);
    cmd.AddValue("txQueueMaxP", "CSMA device TX queue MaxSize (packets)", txQueueMaxP);
    cmd.AddValue("qdiscPollMs", "Queue poll period in milliseconds", qdiscPollMs);
    cmd.AddValue("qdiscOnSwitch", "Install PfifoFast on switch ports too", qdiscOnSwitch);
    cmd.AddValue("qosMark", "QKD priority mark: EF or CS6", qosMark);
    cmd.AddValue("realisticController", "Use realistic control plane delays", realisticController);
    cmd.AddValue("enableLinkFailures", "Enable link failure simulation for robustness testing", enableLinkFailures);
    cmd.Parse(argc, argv);

    // Set deterministic seed
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // Map QoS marking to TOS value
    uint8_t qosTos = (qosMark == "EF" ? 0xB8 : 0xC0); // EF=0xb8, CS6=0xc0
    std::cout << "QKD control traffic using " << qosMark << " marking (TOS=0x" 
              << std::hex << (uint32_t)qosTos << std::dec << ")" << std::endl;

    // Build topology
    TopoBuild topo;
    bool usingCsv = !topoPath.empty();
    Man man;

    if (usingCsv) {
      topo = BuildFromCsv(topoPath, txQueueMaxP);
    } else {
      man = BuildMan(nCore, "10Gbps", "0.5ms", "1Gbps", "0.2ms", enableRing, txQueueMaxP);
    }
    
    // Initialize link failure module
    g_linkFailures.EnableFailures(enableLinkFailures);
    if (enableLinkFailures) {
      std::cout << "Link failure simulation ENABLED - will test network robustness" << std::endl;
    }
    
    // --- Create the proactive controller and wire topology info ---
    Ptr<Node> controllerNode = CreateObject<Node>();
    Ptr<SpProactiveController> ctrl;
    
    if (realisticController) {
      ctrl = CreateObject<RealisticController>();
      std::cout << "Using RealisticController with control-plane latency emulation" << std::endl;
    } else {
      ctrl = CreateObject<SpProactiveController>();
      std::cout << "Using basic SpProactiveController (no latency emulation)" << std::endl;
    }

    // Install controller and switches then open channels
    Ptr<OFSwitch13InternalHelper> of13 = CreateObject<OFSwitch13InternalHelper>();

    // Build network device collections first
    NetDeviceContainer hostDevs;
    std::vector< NetDeviceContainer > spurLinks;
    std::vector< NetDeviceContainer > coreLinks;
    std::vector<Ptr<Node>> hostByIndex;  // CSV mode: nodes in IP assignment order

    // Map to track device -> (switch nodeId, port) for robust adjacency building
    std::unordered_map<Sw, NetDeviceContainer> swPorts; // nodeId -> ports (in exact order passed)
    std::map<Ptr<NetDevice>, std::pair<uint32_t /*nodeId*/, Port>> devToPortByNode;
    std::map<Ptr<NetDevice>, Ipv4Address> devToIp;

    if (usingCsv) {
      // Build stable host index mapping for proper IP assignment
      std::vector<std::pair<uint32_t, Ptr<NetDevice>>> hostNicByIndex;
      for (const auto& e : topo.spurEdges) {
        if (IsHostName(e.nameA)) {
          uint32_t idx = std::stoul(e.nameA.substr(1));
          hostNicByIndex.emplace_back(idx, e.devs.Get(0));
        } else if (IsHostName(e.nameB)) {
          uint32_t idx = std::stoul(e.nameB.substr(1));
          hostNicByIndex.emplace_back(idx, e.devs.Get(1));
        } else {
          NS_FATAL_ERROR("spur edge without a host: " << e.nameA << "," << e.nameB);
        }
        spurLinks.push_back(e.devs);
      }
      
      // Sort by host index to ensure stable IP assignment
      std::sort(hostNicByIndex.begin(), hostNicByIndex.end(),
                [](const auto& a, const auto& b){ return a.first < b.first; });
      
      // Detect host multi-homing (not supported)
      std::unordered_set<uint32_t> seen;
      for (const auto& kv : hostNicByIndex) {
        if (!seen.insert(kv.first).second) {
          NS_FATAL_ERROR("Host h" << kv.first << " appears on multiple spur edges; multi-homing not supported.");
        }
      }
      
      for (const auto& [idx, dev] : hostNicByIndex) {
        hostDevs.Add(dev);
        hostByIndex.push_back(dev->GetNode());   // node order aligned to IPs
      }
      
      for (const auto& e : topo.coreEdges) coreLinks.push_back(e.devs);
    } else {
      for (auto& hl : man.hostLinks) { hostDevs.Add(hl.Get(0)); spurLinks.push_back(hl); }
      for (auto& cl : man.coreLinks) coreLinks.push_back(cl);
    }

    // Register core links with failure module for robustness testing
    if (enableLinkFailures) {
      for (size_t i = 0; i < coreLinks.size(); ++i) {
        std::string desc = usingCsv ? 
          ("CSV_Core_Link_" + std::to_string(i)) : 
          ("Line_Core_Link_" + std::to_string(i));
        g_linkFailures.RegisterLink(coreLinks[i], desc);
      }
    }

    if (usingCsv) {
      // For each switch node, collect its ports from edges
      for (auto swNode : topo.sw) {
        NetDeviceContainer ports;
        for (const auto& e : topo.spurEdges) {
          if (e.b == swNode) ports.Add(e.devs.Get(1));   // host(h) --0/1--> switch(s)
          else if (e.a == swNode) ports.Add(e.devs.Get(0));
        }
        for (const auto& e : topo.coreEdges) {
          if (e.a == swNode) ports.Add(e.devs.Get(0));
          if (e.b == swNode) ports.Add(e.devs.Get(1));
        }
        
        // CSV guardrail: ensure each switch has ≥1 port
        NS_ABORT_MSG_IF(ports.GetN() == 0,
          "CSV switch nodeId=" << swNode->GetId() << " has no attached ports");
        
        of13->InstallSwitch(swNode, ports);
        
        // Record nodeId -> ports mapping (using nodeId as temporary key)
        swPorts[swNode->GetId()] = ports;
      }
    } else {
      // Built-in topology: wire switch ports correctly for line/ring
      for (uint32_t i = 0; i < nCore; i++) {
        NetDeviceContainer ports;
        // Host port (always present)
        ports.Add(man.hostLinks[i].Get(1));
        
        // Core ports for line topology
        for (uint32_t j = 0; j < man.coreLinks.size(); j++) {
          // Check if this switch is connected to core link j
          // For line: link j connects switch j to switch j+1
          if (j == i && i < nCore - 1) {
            // This switch is the 'left' end of link j
            ports.Add(man.coreLinks[j].Get(0));
          }
          if (j == i - 1 && i > 0) {
            // This switch is the 'right' end of link j
            ports.Add(man.coreLinks[j].Get(1));
          }
          // Ring closure link (if enabled)
          if (enableRing && j == nCore - 1 && nCore > 2) {
            if (i == 0) ports.Add(man.coreLinks[j].Get(1)); // link nCore-1 connects switch nCore-1 to switch 0
            if (i == nCore - 1) ports.Add(man.coreLinks[j].Get(0));
          }
        }
        
        of13->InstallSwitch(man.sw[i], ports);
        std::cout << "DEBUG: Installed switch " << i << " (nodeId=" << man.sw[i]->GetId() << ") with " << ports.GetN() << " ports" << std::endl;
        
        // Record nodeId -> ports mapping (using nodeId as temporary key)
        swPorts[man.sw[i]->GetId()] = ports;
      }
    }

    // Build device -> (nodeId, port) index
    for (auto &kv : swPorts) {
      uint32_t nodeId = kv.first;                 // temporary key = switch NodeId
      const auto &plist = kv.second;
      for (uint32_t i = 0; i < plist.GetN(); ++i) {
        devToPortByNode[ plist.Get(i) ] = { nodeId, Port(i+1) }; // OpenFlow ports are 1-based
      }
    }

    // (B) Install controller object (no learning controller)
    of13->InstallController(controllerNode, ctrl);
    std::cout << "DEBUG: Installed controller" << std::endl;

    // (C) Install Internet + assign IPs
    InternetStackHelper internet;
    NodeContainer allHosts;

    if (usingCsv) {
      for (auto h : topo.host) allHosts.Add(h);
    } else {
      for (auto& h : man.host) allHosts.Add(h);
    }
    internet.Install(allHosts);
    
    Ipv4AddressHelper ipv4; 
    ipv4.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer ifs = ipv4.Assign(hostDevs);

    // Build device -> IP mapping for host NICs (so we never rely on spur order)
    for (uint32_t k = 0; k < hostDevs.GetN(); ++k) {
      devToIp[ hostDevs.Get(k) ] = ifs.GetAddress(k);
    }

    // (D) Build dev/port maps + adjacency + hosts (with NodeId keys initially)
    SwAdj adj; 
    std::vector<HostInfo> hostTbl;
    BuildAdjAndHosts_Generic(usingCsv, coreLinks, spurLinks, devToPortByNode, devToIp, adj, hostTbl);

    // Discover DPIDs and remap topology (devices already exist after InstallSwitch)
    std::vector<Ptr<Node>> swNodes = usingCsv ? topo.sw : man.sw;
    auto nodeToDpid = BuildNodeToDpid(swNodes);
    SwAdj adjDpid; 
    std::vector<HostInfo> hostsDpid;
    RemapToDpid(adj, hostTbl, nodeToDpid, adjDpid, hostsDpid);

    // Set DPID-keyed topology on controller (no remapping needed)
    ctrl->SetAdjacency(adjDpid);
    ctrl->SetHosts(hostsDpid);
    std::cout << "DEBUG: Configured controller with " << adjDpid.size() << " switches and " << hostsDpid.size() << " hosts (DPID keys)" << std::endl;

    // Now open channels; HandshakeSuccessful will push flows immediately
    of13->CreateOpenFlowChannels();
    std::cout << "DEBUG: Created OpenFlow channels" << std::endl;

    // Schedule link failure scenarios for robustness testing
    if (enableLinkFailures) {
      g_linkFailures.ScheduleRealisticFailures();
    }

    // Install FlowMonitor for per-flow telemetry
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    // ---------------- QoS: ensure PfifoFast (priority-aware) on host NICs ----------------
    TrafficControlHelper tch;
    if (PfifoHasLimitAttr()) {
      tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc",
                           "Limit", UintegerValue(qdiscMaxP));
    } else {
      tch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
      NS_LOG_INFO("PfifoFast has no 'Limit' attribute in this build; using defaults");
    }

    QueueDiscContainer hostQdiscs;

    for (uint32_t i = 0; i < hostDevs.GetN(); ++i) {
      Ptr<NetDevice> dev = hostDevs.Get(i);
      Ptr<TrafficControlLayer> tcl = dev->GetNode()->GetObject<TrafficControlLayer>();
      
      // Always delete existing qdisc to ensure fresh installation with correct parameters
      if (Ptr<QueueDisc> existing = tcl ? tcl->GetRootQueueDiscOnDevice(dev) : nullptr) {
        tcl->DeleteRootQueueDiscOnDevice(dev);
      }
      
      // Install fresh PfifoFast with specified Limit
      QueueDiscContainer c = tch.Install(NetDeviceContainer(dev));
      hostQdiscs.Add(c.Get(0));
    }

    // Attach telemetry with tags
    for (uint32_t i = 0; i < hostQdiscs.GetN(); ++i) {
      Ptr<QueueDisc> qd = hostQdiscs.Get(i);
      std::string tag = "node" + std::to_string(hostDevs.Get(i)->GetNode()->GetId()) +
                        "/dev" + std::to_string(hostDevs.Get(i)->GetIfIndex());
      qd->TraceConnectWithoutContext("PacketsInQueue", MakeBoundCallback(&QdLenTagged, tag));
      qd->TraceConnectWithoutContext("Drop",           MakeBoundCallback(&QdDropTagged, tag));
    }

    // Object-based queue size polling for verification
    Simulator::Schedule(Seconds(0.4), &PollQ, hostQdiscs, MilliSeconds(qdiscPollMs));

    // Optional: Install PfifoFast on switch ports for egress contention modeling
    if (qdiscOnSwitch) {
      TrafficControlHelper swTch;
      if (PfifoHasLimitAttr()) {
        swTch.SetRootQueueDisc("ns3::PfifoFastQueueDisc",
                               "Limit", UintegerValue(qdiscMaxP));
      } else {
        swTch.SetRootQueueDisc("ns3::PfifoFastQueueDisc");
      }
      
      for (const auto& kv : swPorts) {
        const auto& plist = kv.second;
        for (uint32_t i = 0; i < plist.GetN(); ++i) {
          Ptr<NetDevice> dev = plist.Get(i);
          Ptr<TrafficControlLayer> tcl = dev->GetNode()->GetObject<TrafficControlLayer>();
          
          // Always delete existing qdisc to ensure fresh installation
          if (Ptr<QueueDisc> existing = tcl ? tcl->GetRootQueueDiscOnDevice(dev) : nullptr) {
            tcl->DeleteRootQueueDiscOnDevice(dev);
          }
          
          // Install fresh PfifoFast on switch port
          swTch.Install(NetDeviceContainer(dev));
        }
      }
      std::cout << "DEBUG: Installed PfifoFast on all switch ports for egress contention modeling" << std::endl;
    }

    // Best-effort background traffic (deterministic pairing)
    uint16_t bePort = 9000;
    ApplicationContainer sinkApps;
    
    uint32_t numHosts = usingCsv ? hostByIndex.size() : nCore;
    
    // Safety check: ensure QKD source/destination are valid
    NS_ABORT_MSG_IF(qSrc >= numHosts || qDst >= numHosts,
      "qSrc/qDst out of range for number of hosts (" << numHosts << ")");
    
    // Safety check: ensure even host count for proper pairing
    NS_ABORT_MSG_IF(numHosts % 2 != 0,
      "Pairing requires an even number of hosts (j=(i+numHosts/2)%numHosts).");
    
    for (uint32_t i = 0; i < numHosts; i++) {
      uint32_t j = (i + numHosts/2) % numHosts;  // Deterministic pairing
      
      OnOffHelper onoff("ns3::UdpSocketFactory", 
                        InetSocketAddress(ifs.GetAddress(j), bePort));
      onoff.SetAttribute("DataRate", StringValue(beRate));
      onoff.SetAttribute("PacketSize", UintegerValue(1200));
      onoff.SetAttribute("StartTime", TimeValue(Seconds(0.5 + 0.01*i)));
      // Make BE traffic truly constant (no random on/off periods)
      onoff.SetAttribute("OnTime", StringValue("ns3::ConstantRandomVariable[Constant=1e9]"));
      onoff.SetAttribute("OffTime", StringValue("ns3::ConstantRandomVariable[Constant=0]"));
      
      if (usingCsv) {
        onoff.Install(hostByIndex[i]);
      } else {
        onoff.Install(man.host[i]);
      }
    }
    
    // Packet sinks for best-effort traffic
    PacketSinkHelper sink("ns3::UdpSocketFactory", 
                        InetSocketAddress(Ipv4Address::GetAny(), bePort));
    for (uint32_t j = 0; j < numHosts; j++) {
      if (usingCsv) {
        sinkApps.Add(sink.Install(hostByIndex[j]));
      } else {
        sinkApps.Add(sink.Install(man.host[j]));
      }
    }

    // ARP warmup: prime caches to avoid losing first QKD packets
    V4PingHelper warm(ifs.GetAddress(qDst));
    warm.SetAttribute("StartTime", TimeValue(Seconds(0.2)));
    if (usingCsv) {
      warm.Install(hostByIndex[qSrc]);
    } else {
      warm.Install(man.host[qSrc]);
    }

    // --- old bulk EF QKD window (commented out) ---
    // Ptr<QkdWindowApp> qkd = CreateObject<QkdWindowApp>();
    // if (usingCsv) {
    //   hostByIndex[qSrc]->AddApplication(qkd);
    //   qkd->Configure(hostByIndex[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
    //                 Seconds(qkdStart), Seconds(qkdDur),
    //                 /*pktSize*/ 400, /*pps*/ qkdPps);
    // } else {
    //   man.host[qSrc]->AddApplication(qkd);
    //   qkd->Configure(man.host[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
    //                 Seconds(qkdStart), Seconds(qkdDur),
    //                 /*pktSize*/ 400, /*pps*/ qkdPps);
    // }

    // QKD control trickle (out-of-band model): small UDP from qSrc -> qDst with configurable marking
    Ptr<QkdWindowApp> qctrlApp = CreateObject<QkdWindowApp>();
    ( usingCsv ? hostByIndex[qSrc] : man.host[qSrc] )->AddApplication(qctrlApp);

    // 64 kbps @ 200B -> ~40 pps
    qctrlApp->Configure( usingCsv ? hostByIndex[qSrc] : man.host[qSrc],
                         ifs.GetAddress(qDst), 5555,
                         Seconds(qkdStart),
                         Seconds(15.0 - qkdStart),   // Extended to match simulation duration
                         200, 40.0, qosTos );

    PacketSinkHelper qctrlSink("ns3::UdpSocketFactory",
                               InetSocketAddress(Ipv4Address::GetAny(), 5555));
    ApplicationContainer qctrlSinkApp = (usingCsv
      ? qctrlSink.Install(hostByIndex[qDst])
      : qctrlSink.Install(man.host[qDst]));

    // Schedule error summary for simulation teardown
    if (g_verbosity >= 1) {
      Simulator::ScheduleDestroy([]{
        if (g_dpctlErrors > 0) {
          NS_LOG_ERROR("TEARDOWN: " << g_dpctlErrors << " dpctl errors occurred during simulation");
        } else {
          NS_LOG_INFO("TEARDOWN: No dpctl errors detected");
        }
      });
    }

    Simulator::Stop(Seconds(enableLinkFailures ? 120.0 : 15.0));  // Extended time for link failure scenarios
    Simulator::Run();
    
    // FlowMonitor telemetry output
    fm->CheckForLostPackets();
    fm->SerializeToXmlFile("flows.xml", true, true);
    
    // Quick FlowMonitor summary
    for (const auto& kv : fm->GetFlowStats()) {
      const auto& s = kv.second;
      double dur = 0.0;
      if (s.rxPackets > 1) {
        dur = (s.timeLastRxPacket - s.timeFirstRxPacket).GetSeconds();
      } else {
        dur = (s.timeLastRxPacket - s.timeFirstTxPacket).GetSeconds();
      }
      double thr = dur > 1e-9 ? (s.rxBytes * 8.0) / dur : 0.0;
      std::cout << "Flow " << kv.first << ": rxBytes=" << s.rxBytes
                << " thr=" << thr << " bps"
                << " lost=" << s.lostPackets << "\n";
    }
    
    // Results
    uint64_t totalBeRx = 0;
    for (uint32_t i = 0; i < sinkApps.GetN(); i++) {
      totalBeRx += DynamicCast<PacketSink>(sinkApps.Get(i))->GetTotalRx();
    }
    auto qctrlRx = DynamicCast<PacketSink>(qctrlSinkApp.Get(0))->GetTotalRx();
    
    // Print control plane metrics if using realistic controller
    if (realisticController) {
      ControlPlaneMetrics::PrintSummary();
    }
    
    // Print link failure summary if failures were enabled
    if (enableLinkFailures) {
      g_linkFailures.PrintFailureSummary();
    }
    
    if (usingCsv) {
      std::cout << "MAN from CSV: " << topoPath << " (" << topo.host.size() << " hosts)\n";
    } else {
      std::cout << "MAN with " << nCore << " core switches\n";
    }
    std::cout << "Total BE RX: " << totalBeRx << " bytes\n";
    std::cout << "QKD control RX (host" << qSrc << "→host" << qDst << "): " << qctrlRx << " bytes\n";
    
    Simulator::Destroy();
    return 0;
  }

