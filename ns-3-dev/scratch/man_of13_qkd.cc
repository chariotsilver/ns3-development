#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/internet-apps-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/ofswitch13-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/random-variable-stream.h"
#include "ns3/rng-seed-manager.h"
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <iostream>

using namespace ns3;

// Telemetry callbacks for queue monitoring
void QdLen(std::string ctx, uint32_t oldVal, uint32_t newVal) {
  std::cout << Simulator::Now().GetSeconds() << ",QLEN," << ctx << "," << newVal << "\n";
}

void QdDrop(std::string ctx, Ptr<const QueueDiscItem> item) {
  std::cout << Simulator::Now().GetSeconds() << ",DROP," << ctx << ",1\n";
}

  class QkdWindowApp : public ns3::Application {
  public:
    void Configure(Ptr<Node> n, Ipv4Address dst, uint16_t dport,
                  Time start, Time dur, uint32_t pktSize, double pps) {
      m_node=n; m_dst=dst; m_dport=dport; m_start=start; m_dur=dur;
      m_pktSize=pktSize; m_interval=Seconds(1.0/pps);
    }
  private:
    void StartApplication() override {
      m_sock = Socket::CreateSocket(m_node, UdpSocketFactory::GetTypeId());
      m_sock->SetPriority(6);   // High priority band for reliable classification
      m_sock->SetIpTos(0xb8);   // DSCP EF (complementary)
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
  };

  static NetDeviceContainer Link(Ptr<Node>a, Ptr<Node>b, std::string rate, std::string delay)
  {
    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue(rate));
    csma.SetChannelAttribute("Delay",   StringValue(delay));
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

  TopoBuild BuildFromCsv(const std::string& path)
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
      Ptr<Node> nu = getNode(u), nv = getNode(v);
      auto devs = Link(nu, nv, rate, delay);
      Edge e; e.a=nu; e.b=nv; e.devs=devs; e.kind=kind;
      e.nameA = u; e.nameB = v;  // Track node names for robust device selection
      if (kind=="core") tb.coreEdges.push_back(e); else tb.spurEdges.push_back(e);
    }
    return tb;
  }

  Man BuildMan(uint32_t nCore, std::string coreRate, std::string coreDelay,
              std::string spurRate, std::string spurDelay, bool enableRing = false)
  {
    Man m; 
    m.sw.resize(nCore); 
    m.host.resize(nCore);
    
    // Create nodes
    for (uint32_t i = 0; i < nCore; i++) { 
      m.sw[i] = CreateObject<Node>(); 
      m.host[i] = CreateObject<Node>(); 
    }
    
    // Host spur links
    for (uint32_t i = 0; i < nCore; i++) {
      m.hostLinks.push_back(Link(m.host[i], m.sw[i], spurRate, spurDelay));
    }
    
    // Core topology: line by default, ring only if requested
    for (uint32_t i = 0; i < nCore - 1; i++) {
      auto a = m.sw[i];
      auto b = m.sw[i + 1];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay));
    }
    
    // Close the ring only if explicitly enabled AND we have >2 switches
    if (enableRing && nCore > 2) {
      auto a = m.sw[nCore - 1];
      auto b = m.sw[0];
      m.coreLinks.push_back(Link(a, b, coreRate, coreDelay));
    }
    
    return m;
  }

  int main(int argc, char** argv)
  {
    // Reproducibility
    uint32_t seed = 1, run = 1;
    
    // MAN parameters
    uint32_t nCore = 8;
    
    // QKD parameters
    uint32_t qSrc = 0, qDst = 1;
    double qkdStart = 1.0, qkdDur = 1.5; 
    uint32_t qkdPps = 100000;
    
    // Best-effort parameters
    std::string beRate = "50Mbps";
    
    // Topology parameters
    std::string topoPath = "";
    bool enableRing = false;
    
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
    cmd.Parse(argc, argv);

    // Set deterministic seed
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // Build topology
    TopoBuild topo;
    bool usingCsv = !topoPath.empty();
    Man man;

    if (usingCsv) {
      topo = BuildFromCsv(topoPath);
    } else {
      man = BuildMan(nCore, "10Gbps", "0.5ms", "1Gbps", "0.2ms", enableRing);
    }
    
    // OFSwitch13 setup with controller node
    Ptr<Node> controllerNode = CreateObject<Node>();
    Ptr<OFSwitch13InternalHelper> of13 = CreateObject<OFSwitch13InternalHelper>();
    of13->InstallController(controllerNode);

    // Build network device collections first
    NetDeviceContainer hostDevs;
    std::vector< NetDeviceContainer > spurLinks;
    std::vector< NetDeviceContainer > coreLinks;
    std::vector<Ptr<Node>> hostByIndex;  // CSV mode: nodes in IP assignment order

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
      
      for (const auto& [idx, dev] : hostNicByIndex) {
        hostDevs.Add(dev);
        hostByIndex.push_back(dev->GetNode());   // node order aligned to IPs
      }
      
      for (const auto& e : topo.coreEdges) coreLinks.push_back(e.devs);
    } else {
      for (auto& hl : man.hostLinks) { hostDevs.Add(hl.Get(0)); spurLinks.push_back(hl); }
      for (auto& cl : man.coreLinks) coreLinks.push_back(cl);
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
        of13->InstallSwitch(swNode, ports);
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
      }
    }
    of13->CreateOpenFlowChannels();

    // IP on hosts (this might automatically install default traffic control)
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

    // Install FlowMonitor for per-flow telemetry
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    // ---------------- QoS: install FQ-CoDel for smart queue management -------------
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FqCoDelQueueDisc");   // Good default for mixed UDP/TCP; reduces standing queues

    // deterministic identity for a NIC
    struct DevKey {
      uint32_t node;
      uint32_t ifidx;
      bool operator<(const DevKey& o) const {
        return (node < o.node) || (node == o.node && ifidx < o.ifidx);
      }
    };

    std::set<DevKey> done;

    auto safeInstall = [&](Ptr<NetDevice> dev)
    {
      DevKey k{ dev->GetNode()->GetId(), dev->GetIfIndex() };

      // Check if a root qdisc is already present on this device
      Ptr<TrafficControlLayer> tcl = dev->GetNode()->GetObject<TrafficControlLayer>();
      Ptr<QueueDisc> qdisc = tcl ? tcl->GetRootQueueDiscOnDevice(dev) : nullptr;
      bool hasQdisc = (qdisc != nullptr);

      if (hasQdisc) {
        std::cout << "SKIP (already has qdisc) node " << k.node << " if " << k.ifidx << "\n";
        return;
      }

      if (done.insert(k).second) {
        NetDeviceContainer c; c.Add(dev);
        tch.Install(c);
        std::cout << "TC -> node " << k.node << " if " << k.ifidx << "\n";
      } else {
        std::cout << "SKIP (dup key) node " << k.node << " if " << k.ifidx << "\n";
      }
    };

    // Step A: Start with host-only TC first (zero-risk staging)
    for (auto& hl : spurLinks) {
      safeInstall(hl.Get(0)); // host NIC only
    }

    // Step B: Add core link ends for priority within the fabric
    for (auto& cl : coreLinks) {
      safeInstall(cl.Get(0));
      safeInstall(cl.Get(1));
    }

    // Step C: Add switch-side spur links for complete coverage
    for (auto& hl : spurLinks) {
      safeInstall(hl.Get(1)); // switch NIC
    }

    std::cout << "TC installed on " << done.size() << " devices (comprehensive)\n";

    // Connect queue monitoring callbacks for telemetry
    Config::Connect("/NodeList/*/$ns3::TrafficControlLayer/RootQueueDiscList/*/PacketsInQueue",
                    MakeCallback(&QdLen));
    Config::Connect("/NodeList/*/$ns3::TrafficControlLayer/RootQueueDiscList/*/Drop",
                    MakeCallback(&QdDrop));

    // Best-effort background traffic (deterministic pairing)
    uint16_t bePort = 9000;
    ApplicationContainer sinkApps;
    
    uint32_t numHosts = usingCsv ? hostByIndex.size() : nCore;
    
    // Safety check: ensure QKD source/destination are valid
    NS_ABORT_MSG_IF(qSrc >= numHosts || qDst >= numHosts,
      "qSrc/qDst out of range for number of hosts (" << numHosts << ")");
    
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
    V4PingHelper warm(ifs.GetAddress((qSrc+1) % numHosts));
    warm.SetAttribute("StartTime", TimeValue(Seconds(0.2)));
    if (usingCsv) {
      warm.Install(hostByIndex[qSrc]);
    } else {
      warm.Install(man.host[qSrc]);
    }

    // QKD window: high-priority EF traffic
    Ptr<QkdWindowApp> qkd = CreateObject<QkdWindowApp>();
    if (usingCsv) {
      hostByIndex[qSrc]->AddApplication(qkd);
      qkd->Configure(hostByIndex[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
                    Seconds(qkdStart), Seconds(qkdDur),
                    /*pktSize*/ 400, /*pps*/ qkdPps);
    } else {
      man.host[qSrc]->AddApplication(qkd);
      qkd->Configure(man.host[qSrc], ifs.GetAddress(qDst), /*dport*/ 5555,
                    Seconds(qkdStart), Seconds(qkdDur),
                    /*pktSize*/ 400, /*pps*/ qkdPps);
    }
                  
    PacketSinkHelper qkds("ns3::UdpSocketFactory", 
                        InetSocketAddress(Ipv4Address::GetAny(), 5555));
    ApplicationContainer qkdsink;
    if (usingCsv) {
      qkdsink = qkds.Install(hostByIndex[qDst]);
    } else {
      qkdsink = qkds.Install(man.host[qDst]);
    }

    Simulator::Stop(Seconds(3.0));
    Simulator::Run();
    
    // FlowMonitor telemetry output
    fm->CheckForLostPackets();
    fm->SerializeToXmlFile("flows.xml", true, true);
    
    // Results
    uint64_t totalBeRx = 0;
    for (uint32_t i = 0; i < sinkApps.GetN(); i++) {
      totalBeRx += DynamicCast<PacketSink>(sinkApps.Get(i))->GetTotalRx();
    }
    auto qkdRx = DynamicCast<PacketSink>(qkdsink.Get(0))->GetTotalRx();
    
    if (usingCsv) {
      std::cout << "MAN from CSV: " << topoPath << " (" << topo.host.size() << " hosts)\n";
    } else {
      std::cout << "MAN with " << nCore << " core switches\n";
    }
    std::cout << "Total BE RX: " << totalBeRx << " bytes\n";
    std::cout << "QKD RX (host" << qSrc << "→host" << qDst << "): " << qkdRx << " bytes\n";
    
    Simulator::Destroy();
    return 0;
  }

